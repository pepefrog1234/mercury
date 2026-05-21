/** ffaudio: CoreAudio wrapper
2020, Simon Zolin
*/

#include <ffaudio/audio.h>
#include <ffaudio/util.h>
#include <ffbase/ring.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CFString.h>


int ffcoreaudio_init(ffaudio_init_conf *conf)
{
	return 0;
}

void ffcoreaudio_uninit()
{
}


struct ffaudio_dev {
	ffuint mode;
	ffuint idev;
	ffuint ndev;
	AudioObjectID *devs;
	char *name;

	ffuint err;
	char *errmsg;
};

ffaudio_dev* ffcoreaudio_dev_alloc(ffuint mode)
{
	ffaudio_dev *d = ffmem_new(ffaudio_dev);
	if (d == NULL)
		return NULL;
	d->mode = mode;
	return d;
}

void ffcoreaudio_dev_free(ffaudio_dev *d)
{
	if (d == NULL)
		return;
	ffmem_free(d->errmsg);
	ffmem_free(d->devs);
	ffmem_free(d->name);
	ffmem_free(d);
}

static const AudioObjectPropertyAddress prop_dev_list = {
	kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMaster
};
static const AudioObjectPropertyAddress prop_dev_outname = {
	kAudioObjectPropertyName, kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMaster
};
static const AudioObjectPropertyAddress prop_dev_inname = {
	kAudioObjectPropertyName, kAudioDevicePropertyScopeInput, kAudioObjectPropertyElementMaster
};
static const AudioObjectPropertyAddress prop_dev_outconf = {
	kAudioDevicePropertyStreamConfiguration, kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMaster
};
static const AudioObjectPropertyAddress prop_dev_inconf = {
	kAudioDevicePropertyStreamConfiguration, kAudioDevicePropertyScopeInput, kAudioObjectPropertyElementMaster
};

/** Get device list */
static int coreaudio_dev_list(ffaudio_dev *d)
{
	int rc = FFAUDIO_ERROR;
	OSStatus r;
	ffuint size;
	r = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &prop_dev_list, 0, NULL, &size);
	if (r != kAudioHardwareNoError)
		return 0;

	if (NULL == (d->devs = ffmem_alloc(size))) {
		d->err = errno;
		return FFAUDIO_ERROR;
	}
	r = AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop_dev_list, 0, NULL, &size, d->devs);
	if (r != kAudioHardwareNoError) {
		d->err = r;
		goto end;
	}
	d->ndev = size / sizeof(AudioObjectID);

	rc = 0;

end:
	if (rc != 0) {
		ffmem_free(d->devs);
		d->devs = NULL;
	}
	return rc;
}

/** Get name of the current device. */
static int coreaudio_dev_name(ffaudio_dev *d)
{
	int rc = FFAUDIO_ERROR;
	const AudioObjectPropertyAddress *prop;
	AudioBufferList *bufs = NULL;
	CFStringRef cfs = NULL;
	OSStatus r;
	ffuint size;

	d->err = 0;

	prop = (d->mode == FFAUDIO_DEV_CAPTURE) ? &prop_dev_inconf : &prop_dev_outconf;
	r = AudioObjectGetPropertyDataSize(d->devs[d->idev], prop, 0, NULL, &size);
	if (r != kAudioHardwareNoError)
		goto end;

	if (NULL == (bufs = ffmem_alloc(size))) {
		d->err = errno;
		rc = FFAUDIO_ERROR;
		goto end;
	}
	r = AudioObjectGetPropertyData(d->devs[d->idev], prop, 0, NULL, &size, bufs);
	if (r != kAudioHardwareNoError)
		goto end;

	ffuint ch = 0;
	for (ffuint i = 0;  i != bufs->mNumberBuffers;  i++) {
		ch |= bufs->mBuffers[i].mNumberChannels;
	}
	if (ch == 0)
		goto end;

	size = sizeof(CFStringRef);
	prop = (d->mode == FFAUDIO_DEV_CAPTURE) ? &prop_dev_inname : &prop_dev_outname;
	r = AudioObjectGetPropertyData(d->devs[d->idev], prop, 0, NULL, &size, &cfs);
	if (r != kAudioHardwareNoError)
		goto end;

	CFIndex len = CFStringGetMaximumSizeForEncoding(CFStringGetLength(cfs), kCFStringEncodingUTF8);
	if (NULL == (d->name = ffmem_alloc(len + 1))) {
		d->err = errno;
		rc = FFAUDIO_ERROR;
		goto end;
	}
	if (!CFStringGetCString(cfs, d->name, len + 1, kCFStringEncodingUTF8))
		goto end;

	rc = 0;

end:
	if (rc == FFAUDIO_ERROR)
		d->err = r;
	ffmem_free(bufs);
	if (rc != 0) {
		ffmem_free(d->name);
		d->name = NULL;
	}
	if (cfs != NULL)
		CFRelease(cfs);
	return rc;
}

int ffcoreaudio_dev_next(ffaudio_dev *d)
{
	if (d->devs == NULL) {
		int r;
		if (0 != (r = coreaudio_dev_list(d)))
			return -r;
	}

	for (;;) {
		ffmem_free(d->name);
		d->name = NULL;

		if (d->idev == d->ndev)
			return 1;

		if (0 != coreaudio_dev_name(d)) {
			d->idev++;
			continue;
		}

		d->idev++;
		return 0;
	}
}

const char* ffcoreaudio_dev_info(ffaudio_dev *d, ffuint i)
{
	switch (i) {
	case FFAUDIO_DEV_ID:
		if (d->idev == 0)
			return NULL;

		return (char*)&d->devs[d->idev - 1];

	case FFAUDIO_DEV_NAME:
		return d->name;
	}
	return NULL;
}

const char* ffcoreaudio_dev_error(ffaudio_dev *d)
{
	ffmem_free(d->errmsg);
	d->errmsg = ffsz_allocfmt("%d (%xu)", d->err, d->err);
	return d->errmsg;
}


struct ffaudio_buf {
	ffuint dev;
	void *aprocid;
	ffring *ring;
	ffuint period_ms;
	ffuint overrun;
	ffuint nonblock;
	ffuint channels;
	ffuint sample_size;
	char *conv;
	ffsize conv_cap;
	ffstr buf_locked;
	ffring_head rhead;

	const char *errfunc;
};

ffaudio_buf* ffcoreaudio_alloc()
{
	ffaudio_buf *b = ffmem_new(ffaudio_buf);
	if (b == NULL)
		return NULL;
	return b;
}

void ffcoreaudio_free(ffaudio_buf *b)
{
	if (b == NULL)
		return;

	ffring_free(b->ring);
	AudioDeviceDestroyIOProcID(b->dev, b->aprocid);
	ffmem_free(b->conv);
	ffmem_free(b);
}

const char* ffcoreaudio_error(ffaudio_buf *b)
{
	return b->errfunc;
}

static OSStatus coreaudio_ioproc_playback(AudioDeviceID device, const AudioTimeStamp *now,
	const AudioBufferList *indata, const AudioTimeStamp *intime,
	AudioBufferList *outdata, const AudioTimeStamp *outtime,
	void *udata);
static OSStatus coreaudio_ioproc_capture(AudioDeviceID device, const AudioTimeStamp *now,
	const AudioBufferList *indata, const AudioTimeStamp *intime,
	AudioBufferList *outdata, const AudioTimeStamp *outtime,
	void *udata);
static const AudioObjectPropertyAddress prop_odev_fmt = {
	kAudioDevicePropertyStreamFormat, kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMaster
};
static const AudioObjectPropertyAddress prop_idev_fmt = {
	kAudioDevicePropertyStreamFormat, kAudioDevicePropertyScopeInput, kAudioObjectPropertyElementMaster
};
static const AudioObjectPropertyAddress prop_dev_nominal_rate = {
	kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMaster
};

static const AudioObjectPropertyAddress prop_idev_default = {
	kAudioHardwarePropertyDefaultInputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMaster
};
static const AudioObjectPropertyAddress prop_odev_default = {
	kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMaster
};
static int coreaudio_dev_default(ffuint capture)
{
	AudioObjectID dev;
	ffuint size = sizeof(AudioObjectID);
	const AudioObjectPropertyAddress *a = (capture) ? &prop_idev_default : &prop_odev_default;
	OSStatus r = AudioObjectGetPropertyData(kAudioObjectSystemObject, a, 0, NULL, &size, &dev);
	if (r != 0)
		return -1;
	return dev;
}

static int coreaudio_device_channels(AudioObjectID dev, ffuint capture)
{
	const AudioObjectPropertyAddress *a = (capture) ? &prop_dev_inconf : &prop_dev_outconf;
	AudioBufferList *bufs = NULL;
	ffuint size = 0;
	int channels = 0;

	if (AudioObjectGetPropertyDataSize(dev, a, 0, NULL, &size) != kAudioHardwareNoError)
		return 0;

	bufs = ffmem_alloc(size);
	if (bufs == NULL)
		return 0;

	if (AudioObjectGetPropertyData(dev, a, 0, NULL, &size, bufs) == kAudioHardwareNoError) {
		for (ffuint i = 0; i != bufs->mNumberBuffers; i++)
			channels += bufs->mBuffers[i].mNumberChannels;
	}

	ffmem_free(bufs);
	return channels;
}

static double coreaudio_device_nominal_rate(AudioObjectID dev)
{
	Float64 rate = 0;
	ffuint size = sizeof(rate);

	if (AudioObjectGetPropertyData(dev, &prop_dev_nominal_rate, 0, NULL,
	                               &size, &rate) != kAudioHardwareNoError)
		return 0;
	return rate;
}

int ffcoreaudio_open(ffaudio_buf *b, ffaudio_conf *conf, ffuint flags)
{
	int rc = FFAUDIO_ERROR;
	ffuint capture = (flags & 0x0f) == FFAUDIO_DEV_CAPTURE;
	b->nonblock = !!(flags & FFAUDIO_O_NONBLOCK);

	int dev = -1;
	if (conf->device_id != NULL)
		dev = *(int*)conf->device_id;
	if (dev < 0) {
		dev = coreaudio_dev_default(capture);
		if (dev < 0) {
			b->errfunc = "get default device";
			return FFAUDIO_ERROR;
		}
	}

	AudioStreamBasicDescription asbd = {};
	ffuint size = sizeof(asbd);
	const AudioObjectPropertyAddress *a = (capture) ? &prop_idev_fmt : &prop_odev_fmt;
	if (0 != AudioObjectGetPropertyData(dev, a, 0, NULL, &size, &asbd)) {
		/* Aggregate/virtual devices and some USB codecs don't expose
		 * kAudioDevicePropertyStreamFormat at device scope.  The IOProc still
		 * supplies float buffers, so fall back to nominal rate + stream layout. */
		asbd.mSampleRate = coreaudio_device_nominal_rate(dev);
		asbd.mChannelsPerFrame = coreaudio_device_channels(dev, capture);
	}
	if (asbd.mSampleRate <= 0)
		asbd.mSampleRate = coreaudio_device_nominal_rate(dev);
	if (asbd.mChannelsPerFrame == 0)
		asbd.mChannelsPerFrame = coreaudio_device_channels(dev, capture);
	if (asbd.mSampleRate <= 0 || asbd.mChannelsPerFrame == 0) {
		b->errfunc = "AudioStreamBasicDescription";
		return -1;
	}

	int new_format = 0;
	if (conf->format != FFAUDIO_F_FLOAT32) {
		conf->format = FFAUDIO_F_FLOAT32;
		new_format = 1;
	}

	if (conf->sample_rate != asbd.mSampleRate) {
		conf->sample_rate = asbd.mSampleRate;
		new_format = 1;
	}

	if (conf->channels != asbd.mChannelsPerFrame) {
		conf->channels = asbd.mChannelsPerFrame;
		new_format = 1;
	}

	if (new_format)
		return FFAUDIO_EFORMAT;

	void *proc = (capture) ? coreaudio_ioproc_capture : coreaudio_ioproc_playback;
	if (0 != AudioDeviceCreateIOProcID(dev, proc, b, (AudioDeviceIOProcID*)&b->aprocid)
		|| b->aprocid == NULL) {
		b->errfunc = "AudioDeviceCreateIOProcID";
		goto end;
	}

	if (conf->buffer_length_msec == 0)
		conf->buffer_length_msec = 500;
	ffuint bufsize = _ffau_buf_msec_to_size(conf, conf->buffer_length_msec);
	if (NULL == (b->ring = ffring_alloc(bufsize, FFRING_1_READER | FFRING_1_WRITER))) {
		b->errfunc = "ffring_alloc";
		goto end;
	}
	b->period_ms = conf->buffer_length_msec / 4;
	b->channels = conf->channels;
	b->sample_size = (conf->format & 0xff) / 8;

	b->dev = dev;
	rc = 0;

end:
	if (rc != 0)
		AudioDeviceDestroyIOProcID(b->dev, b->aprocid);
	return rc;
}

static int coreaudio_reserve_conv(ffaudio_buf *b, ffsize size)
{
	if (b->conv_cap >= size)
		return 0;

	char *p = (char*)ffmem_realloc(b->conv, size);
	if (p == NULL)
		return -1;
	b->conv = p;
	b->conv_cap = size;
	return 0;
}

static size_t coreaudio_ring_read_bytes(ffaudio_buf *b, char *dst, size_t len)
{
	size_t done = 0;

	while (done != len) {
		ffstr s;
		ffring_head h = ffring_read_begin(b->ring, len - done, &s, NULL);
		if (s.len == 0)
			break;
		memcpy(dst + done, s.ptr, s.len);
		done += s.len;
		ffring_read_finish(b->ring, h);
	}

	if (done != len) {
		memset(dst + done, 0, len - done);
		b->overrun = 1;
	}

	return done;
}

static void coreaudio_ring_write_bytes(ffaudio_buf *b, const char *data, size_t len)
{
	ffuint r = ffring_write(b->ring, data, len);
	if (r != len) {
		r += ffring_write(b->ring, data + r, len - r);
		if (r != len)
			b->overrun = 1;
	}
}

int ffcoreaudio_start(ffaudio_buf *b)
{
	if (0 != AudioDeviceStart(b->dev, b->aprocid)) {
		b->errfunc = "AudioDeviceStart";
		return FFAUDIO_ERROR;
	}
	return 0;
}

int ffcoreaudio_stop(ffaudio_buf *b)
{
	if (0 != AudioDeviceStop(b->dev, b->aprocid)) {
		b->errfunc = "AudioDeviceStop";
		return FFAUDIO_ERROR;
	}
	return 0;
}

int ffcoreaudio_clear(ffaudio_buf *b)
{
	ffring_reset(b->ring);
	return 0;
}

static OSStatus coreaudio_ioproc_playback(AudioDeviceID device, const AudioTimeStamp *now,
	const AudioBufferList *indata, const AudioTimeStamp *intime,
	AudioBufferList *outdata, const AudioTimeStamp *outtime,
	void *udata)
{
	ffaudio_buf *b = udata;
	const ffuint sample_size = b->sample_size ? b->sample_size : sizeof(float);

	if (outdata->mNumberBuffers <= 1) {
		char *d = (char*)outdata->mBuffers[0].mData;
		size_t n = outdata->mBuffers[0].mDataByteSize;
		coreaudio_ring_read_bytes(b, d, n);
		return 0;
	}

	ffuint channels = 0;
	size_t frames = 0;
	for (ffuint bi = 0; bi != outdata->mNumberBuffers; bi++) {
		AudioBuffer *ab = &outdata->mBuffers[bi];
		if (ab->mNumberChannels == 0)
			continue;
		size_t bframes = ab->mDataByteSize / (ab->mNumberChannels * sample_size);
		if (frames == 0 || bframes < frames)
			frames = bframes;
		channels += ab->mNumberChannels;
	}
	if (frames == 0 || channels == 0)
		return 0;

	size_t need = frames * channels * sample_size;
	if (coreaudio_reserve_conv(b, need) != 0) {
		for (ffuint bi = 0; bi != outdata->mNumberBuffers; bi++)
			memset(outdata->mBuffers[bi].mData, 0, outdata->mBuffers[bi].mDataByteSize);
		b->overrun = 1;
		return 0;
	}

	coreaudio_ring_read_bytes(b, b->conv, need);

	ffuint ch0 = 0;
	for (ffuint bi = 0; bi != outdata->mNumberBuffers; bi++) {
		AudioBuffer *ab = &outdata->mBuffers[bi];
		char *dst = (char*)ab->mData;
		ffuint bch = ab->mNumberChannels;
		size_t active = frames * bch * sample_size;
		for (size_t frame = 0; frame != frames; frame++) {
			for (ffuint ch = 0; ch != bch; ch++) {
				memcpy(dst + ((frame * bch + ch) * sample_size),
				       b->conv + ((frame * channels + ch0 + ch) * sample_size),
				       sample_size);
			}
		}
		if (ab->mDataByteSize > active)
			memset(dst + active, 0, ab->mDataByteSize - active);
		ch0 += bch;
	}

	return 0;
}

static int coreaudio_writeonce(ffaudio_buf *b, const void *data, ffsize len)
{
	ffsize n = ffring_write(b->ring, data, len);
	return n;
}

static OSStatus coreaudio_ioproc_capture(AudioDeviceID device, const AudioTimeStamp *now,
	const AudioBufferList *indata, const AudioTimeStamp *intime,
	AudioBufferList *outdata, const AudioTimeStamp *outtime,
	void *udata)
{
	ffaudio_buf *b = udata;
	const ffuint sample_size = b->sample_size ? b->sample_size : sizeof(float);

	if (indata->mNumberBuffers <= 1) {
		const char *d = (const char*)indata->mBuffers[0].mData;
		size_t n = indata->mBuffers[0].mDataByteSize;
		coreaudio_ring_write_bytes(b, d, n);
		return 0;
	}

	ffuint channels = 0;
	size_t frames = 0;
	for (ffuint bi = 0; bi != indata->mNumberBuffers; bi++) {
		const AudioBuffer *ab = &indata->mBuffers[bi];
		if (ab->mNumberChannels == 0)
			continue;
		size_t bframes = ab->mDataByteSize / (ab->mNumberChannels * sample_size);
		if (frames == 0 || bframes < frames)
			frames = bframes;
		channels += ab->mNumberChannels;
	}
	if (frames == 0 || channels == 0)
		return 0;

	size_t need = frames * channels * sample_size;
	if (coreaudio_reserve_conv(b, need) != 0) {
		b->overrun = 1;
		return 0;
	}

	ffuint ch0 = 0;
	for (ffuint bi = 0; bi != indata->mNumberBuffers; bi++) {
		const AudioBuffer *ab = &indata->mBuffers[bi];
		const char *src = (const char*)ab->mData;
		ffuint bch = ab->mNumberChannels;
		for (size_t frame = 0; frame != frames; frame++) {
			for (ffuint ch = 0; ch != bch; ch++) {
				memcpy(b->conv + ((frame * channels + ch0 + ch) * sample_size),
				       src + ((frame * bch + ch) * sample_size),
				       sample_size);
			}
		}
		ch0 += bch;
	}

	coreaudio_ring_write_bytes(b, b->conv, need);
	return 0;
}

static int coreaudio_readonce(ffaudio_buf *b, const void **buffer)
{
	if (b->buf_locked.len != 0) {
		ffring_read_finish(b->ring, b->rhead);
	}

	b->rhead = ffring_read_begin(b->ring, -1, &b->buf_locked, NULL);
	*buffer = b->buf_locked.ptr;
	return b->buf_locked.len;
}

int ffcoreaudio_write(ffaudio_buf *b, const void *data, ffsize len)
{
	for (;;) {
		int r = coreaudio_writeonce(b, data, len);
		if (r != 0)
			return r;

		if (0 != (r = ffcoreaudio_start(b)))
			return r;

		if (b->nonblock)
			return 0;

		usleep(1000);
	}
}

int ffcoreaudio_drain(ffaudio_buf *b)
{
	int r;
	for (;;) {
		ffstr s;
		ffsize free;
		ffring_write_begin(b->ring, 0, &s, &free);

		if (free == b->ring->cap) {
			(void) ffcoreaudio_stop(b);
			return 1;
		}

		if (0 != (r = ffcoreaudio_start(b)))
			return r;

		if (b->nonblock)
			return 0;

		usleep(1000);
	}
}

int ffcoreaudio_read(ffaudio_buf *b, const void **buffer)
{
	for (;;) {
		int r = coreaudio_readonce(b, buffer);
		if (r != 0)
			return r;

		if (0 != (r = ffcoreaudio_start(b)))
			return -r;

		if (b->nonblock)
			return 0;

		usleep(1000);
	}
}


const struct ffaudio_interface ffcoreaudio = {
	ffcoreaudio_init,
	ffcoreaudio_uninit,

	ffcoreaudio_dev_alloc,
	ffcoreaudio_dev_free,
	ffcoreaudio_dev_error,
	ffcoreaudio_dev_next,
	ffcoreaudio_dev_info,

	ffcoreaudio_alloc,
	ffcoreaudio_free,
	ffcoreaudio_error,
	ffcoreaudio_open,
	ffcoreaudio_start,
	ffcoreaudio_stop,
	ffcoreaudio_clear,
	ffcoreaudio_write,
	ffcoreaudio_drain,
	ffcoreaudio_read,
	NULL,
};

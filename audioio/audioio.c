/* Audio subsystem
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@rhizomatica.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */


#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include "os_interop.h"
#include <ffaudio/audio.h>
#include "std.h"
#ifndef FF_WIN
#include <time.h>
#endif

#include "ring_buffer_posix.h"
#include "shm_posix.h"
#include "defines_modem.h"

#include "audioio.h"
#include "hermes_log.h"

extern volatile bool shutdown_;

/* ------------------------------------------------------------------ */
/*  DirectSound GUID ↔ string helpers (Windows only)                  */
/* ------------------------------------------------------------------ */
#if defined(_WIN32)

/* Format a GUID as "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}".
 * buf must be at least 39 bytes (38 chars + NUL).                    */
static void guid_to_str(const GUID *g, char *buf, size_t bufsize)
{
    snprintf(buf, bufsize,
             "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
             (unsigned long)g->Data1, g->Data2, g->Data3,
             g->Data4[0], g->Data4[1],
             g->Data4[2], g->Data4[3], g->Data4[4],
             g->Data4[5], g->Data4[6], g->Data4[7]);
}

/* Parse a GUID string back into a GUID struct.  Returns 0 on success. */
static int str_to_guid(const char *s, GUID *g)
{
    unsigned long d1;
    unsigned int d2, d3;
    unsigned int d4[8];
    if (sscanf(s, "{%8lX-%4X-%4X-%2X%2X-%2X%2X%2X%2X%2X%2X}",
               &d1, &d2, &d3,
               &d4[0], &d4[1], &d4[2], &d4[3],
               &d4[4], &d4[5], &d4[6], &d4[7]) != 11)
        return -1;
    g->Data1 = d1;
    g->Data2 = (unsigned short)d2;
    g->Data3 = (unsigned short)d3;
    for (int i = 0; i < 8; i++)
        g->Data4[i] = (unsigned char)d4[i];
    return 0;
}

static int ascii_equal_ci(const char *a, const char *b)
{
    if (!a || !b)
        return 0;

    while (*a && *b)
    {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (tolower(ca) != tolower(cb))
            return 0;
    }

    return *a == '\0' && *b == '\0';
}

static int audio_device_label_matches(const char *requested, const char *name, const char *id)
{
    size_t name_len;

    if (!requested || !requested[0])
        return 0;

    if (id && id[0] && strcmp(requested, id) == 0)
        return 1;

    if (name && name[0] && (strcmp(requested, name) == 0 || ascii_equal_ci(requested, name)))
        return 1;

    if (name && id && name[0] && id[0])
    {
        name_len = strlen(name);
        if (strncmp(requested, name, name_len) == 0 &&
            strncmp(requested + name_len, " [", 2) == 0 &&
            strstr(requested + name_len, id) != NULL)
            return 1;
    }

    return 0;
}

/* Windows users may select a friendly device name from Qt or a settings file,
 * while WASAPI needs an MMDevice ID and DirectSound needs a GUID.  Accept all
 * three forms to avoid opening no audio device while radio/CAT control still
 * appears to work.  Return 0 when resolved_id was filled, 1 when the matching
 * device is the subsystem default with no ID, and -1 when no match was found. */
static int resolve_windows_audio_device_id(ffaudio_interface *audio,
                                           unsigned mode,
                                           const char *requested,
                                           char *resolved_id,
                                           size_t resolved_id_size)
{
    ffaudio_dev *d;
    int rc = -1;

    if (!audio || !requested || !requested[0] || !resolved_id || resolved_id_size == 0)
        return -1;

    d = audio->dev_alloc(mode);
    if (!d)
        return -1;

    for (;;)
    {
        int r = audio->dev_next(d);
        const char *id;
        const char *name;

        if (r != 0)
            break;

        id = audio->dev_info(d, FFAUDIO_DEV_ID);
        name = audio->dev_info(d, FFAUDIO_DEV_NAME);

        if (!audio_device_label_matches(requested, name, id))
            continue;

        if (id && id[0])
        {
            int written = snprintf(resolved_id, resolved_id_size, "%s", id);
            if (written >= 0 && (size_t)written < resolved_id_size)
                rc = 0;
        }
        else
        {
            resolved_id[0] = '\0';
            rc = 1;
        }
        break;
    }

    audio->dev_free(d);
    return rc;
}

#endif /* _WIN32 */

#if defined(__APPLE__)
static int coreaudio_resolve_device_id(ffaudio_interface *audio, unsigned mode, const char *device_id, int *resolved_id)
{
    char *endptr = NULL;
    long numeric_id;
    ffaudio_dev *d;
    int rc = -1;

    if (!audio || !device_id || !device_id[0] || !resolved_id)
        return -1;

    numeric_id = strtol(device_id, &endptr, 10);
    if (endptr && *endptr == '\0')
    {
        *resolved_id = (int)numeric_id;
        return 0;
    }

    d = audio->dev_alloc(mode);
    if (!d)
        return -1;

    for (;;)
    {
        int r = audio->dev_next(d);
        if (r > 0)
            break;
        if (r < 0)
            break;

        const char *name = audio->dev_info(d, FFAUDIO_DEV_NAME);
        const char *id = audio->dev_info(d, FFAUDIO_DEV_ID);
        if (name && id && strcmp(name, device_id) == 0)
        {
            memcpy(resolved_id, id, sizeof(*resolved_id));
            rc = 0;
            break;
        }
    }

    audio->dev_free(d);
    return rc;
}
#endif /* __APPLE__ */

cbuf_handle_t capture_buffer;
cbuf_handle_t playback_buffer;

int audio_subsystem;
static int capture_input_channel_layout = LEFT;

// Internal state for restart support
static pthread_t s_radio_capture;
static pthread_t s_radio_playback;
static char s_capture_dev[256];
static char s_playback_dev[256];
static int s_buffers_initialized = 0;
static int s_buffers_are_shm = 0;
static volatile bool audio_shutdown_ = false;  // local stop flag for audio threads
static atomic_int s_playback_gain_percent = ATOMIC_VAR_INIT(100);

struct conf {
    const char *cmd;
    ffaudio_conf buf;
    uint8_t flags;
    uint8_t exclusive;
    uint8_t hwdev;
    uint8_t loopback;
    uint8_t nonblock;
    uint8_t wav;
};

#define AUDIOIO_MODEM_SAMPLE_RATE 8000

int audioio_set_playback_gain_percent(int percent)
{
    if (percent < 0)
        percent = 0;
    if (percent > 200)
        percent = 200;
    atomic_store_explicit(&s_playback_gain_percent, percent, memory_order_relaxed);
    return percent;
}

int audioio_get_playback_gain_percent(void)
{
    return atomic_load_explicit(&s_playback_gain_percent, memory_order_relaxed);
}

static int32_t audioio_apply_playback_gain(int32_t sample)
{
    int gain_percent = audioio_get_playback_gain_percent();
    int64_t scaled;

    if (gain_percent == 100)
        return sample;

    scaled = ((int64_t)sample * (int64_t)gain_percent) / 100;
    if (scaled > INT32_MAX)
        return INT32_MAX;
    if (scaled < INT32_MIN)
        return INT32_MIN;
    return (int32_t)scaled;
}

static const char *audioio_format_name(unsigned format)
{
    switch (format)
    {
    case FFAUDIO_F_FLOAT32: return "float32";
    case FFAUDIO_F_INT32:   return "int32";
    case FFAUDIO_F_INT24_4: return "int24in32";
    case FFAUDIO_F_INT24:   return "int24";
    case FFAUDIO_F_INT16:   return "int16";
    default:                return "unknown";
    }
}

static int audioio_rate_ratio(unsigned device_sample_rate, const char *tag)
{
    if (device_sample_rate == 0 ||
        device_sample_rate % AUDIOIO_MODEM_SAMPLE_RATE != 0)
    {
        HLOGE(tag, "Unsupported device sample rate %u Hz; modem audio is %d Hz and currently needs an integer-rate device",
              device_sample_rate, AUDIOIO_MODEM_SAMPLE_RATE);
        return 0;
    }

    int ratio = (int)(device_sample_rate / AUDIOIO_MODEM_SAMPLE_RATE);
    if (ratio <= 0)
    {
        HLOGE(tag, "Invalid device/modem sample-rate ratio: %u/%d",
              device_sample_rate, AUDIOIO_MODEM_SAMPLE_RATE);
        return 0;
    }

    return ratio;
}

static bool audioio_playback_format_supported(unsigned format)
{
    return format == FFAUDIO_F_FLOAT32 ||
           format == FFAUDIO_F_INT32 ||
           format == FFAUDIO_F_INT24_4 ||
           format == FFAUDIO_F_INT16;
}

static void audioio_store_playback_sample(uint8_t *dst, unsigned format, int32_t sample)
{
    switch (format)
    {
    case FFAUDIO_F_FLOAT32:
        *(float *)dst = (float)((double)sample / 2147483648.0);
        break;
    case FFAUDIO_F_INT16:
        *(int16_t *)dst = (int16_t)(sample >> 16);
        break;
    case FFAUDIO_F_INT32:
    case FFAUDIO_F_INT24_4:
    default:
        *(int32_t *)dst = sample;
        break;
    }
}


static inline void ffthread_sleep(ffuint msec)
{
#ifdef FF_WIN
    Sleep(msec);
#else
    struct timespec ts = {
        .tv_sec = msec / 1000,
        .tv_nsec = (msec % 1000) * 1000000,
    };
    nanosleep(&ts, NULL);
#endif
}

static inline uint64_t audioio_monotonic_ms(void)
{
#ifdef FF_WIN
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
#endif
}

#if defined(__linux__)
static bool pulse_init_already_initialized(const ffaudio_init_conf *aconf)
{
    return aconf && aconf->error && strcmp(aconf->error, "already initialized") == 0;
}

static int pulse_shared_init(bool *did_init)
{
    ffaudio_interface *audio = (ffaudio_interface *) &ffpulse;
    ffaudio_init_conf aconf = {};
    aconf.app_name = "mercury";
    if (did_init)
        *did_init = false;

    if (audio->init(&aconf) != 0)
    {
        if (pulse_init_already_initialized(&aconf))
            return 0;
        HLOGE("audio-pulse", "Error initializing PulseAudio: %s",
              aconf.error ? aconf.error : "unknown");
        return -1;
    }

    if (did_init)
        *did_init = true;
    return 0;
}

static void pulse_shared_uninit(void)
{
    ffaudio_interface *audio = (ffaudio_interface *) &ffpulse;
    audio->uninit();
}
#endif

int audioio_pick_default_subsystem(void)
{
#if defined(__linux__)
    return AUDIO_SUBSYSTEM_ALSA;
#elif defined(_WIN32)
    return AUDIO_SUBSYSTEM_WASAPI;
#elif defined(__FREEBSD__)
    return AUDIO_SUBSYSTEM_OSS;
#elif defined(__APPLE__)
    return AUDIO_SUBSYSTEM_COREAUDIO;
#elif defined(__ANDROID__)
    return AUDIO_SUBSYSTEM_AAUDIO;
#else
    return AUDIO_SUBSYSTEM_ALSA;
#endif
}


void *radio_playback_thread(void *device_ptr)
{
    ffaudio_interface *audio = NULL;
    struct conf conf = {};
    int coreaudio_device_id = -1;
#if defined(_WIN32)
    char windows_device_id[2048];
    GUID play_guid;
#endif
    conf.buf.app_name = "mercury_playback";
    conf.buf.format = FFAUDIO_F_INT32;
    conf.buf.sample_rate = 48000;
    conf.buf.channels = 2;
    conf.buf.device_id = (device_ptr && ((const char *)device_ptr)[0] != '\0')
                         ? (const char *) device_ptr : NULL;
    uint32_t period_ms;


#if defined(_WIN32)
    conf.buf.buffer_length_msec = 40;
    period_ms = conf.buf.buffer_length_msec / 4;
    if (audio_subsystem == AUDIO_SUBSYSTEM_WASAPI)
        audio = (ffaudio_interface *) &ffwasapi;
    if (audio_subsystem == AUDIO_SUBSYSTEM_DSOUND)
        audio = (ffaudio_interface *) &ffdsound;
#elif defined(__linux__)
    conf.buf.buffer_length_msec = 30;
    period_ms = conf.buf.buffer_length_msec / 3;
    if (audio_subsystem == AUDIO_SUBSYSTEM_ALSA)
        audio = (ffaudio_interface *) &ffalsa;
    if (audio_subsystem == AUDIO_SUBSYSTEM_PULSE)
        audio = (ffaudio_interface *) &ffpulse;
#elif defined(__FREEBSD__)
    conf.buf.buffer_length_msec = 40;
    period_ms = conf.buf.buffer_length_msec / 4;
    if (audio_subsystem == AUDIO_SUBSYSTEM_OSS)
        audio = (ffaudio_interface *) &ffoss;
#elif defined(__APPLE__)
    conf.buf.buffer_length_msec = 40;
    period_ms = conf.buf.buffer_length_msec / 2;
    if (audio_subsystem == AUDIO_SUBSYSTEM_COREAUDIO)
        audio = (ffaudio_interface *) &ffcoreaudio;
#endif

    if (!audio)
    {
        HLOGE("audio-play", "Unsupported audio subsystem: %d", audio_subsystem);
        return NULL;
    }

    conf.flags = FFAUDIO_PLAYBACK;
    ffaudio_init_conf aconf = {};
    aconf.app_name = "mercury_playback";

    int r;
    ffaudio_buf *b;
    ffaudio_conf *cfg;

    ffuint frame_size;
    ffuint msec_bytes;

    int32_t *input_buffer = NULL;
    int32_t *buffer_upsampled = NULL;
    uint8_t *buffer_output = NULL;

    ffuint total_written = 0;
    int ch_layout = STEREO;
    int resample_ratio = 0;

    /* PulseAudio uses a single global context (gconn in pulse.c).
     * If init() returns "already initialized" it means the capture thread
     * already called init() successfully and we can proceed normally.
     * Track whether we initialized so we only uninit once.
     */
    bool did_init_play = false;
    r = audio->init(&aconf);
    if (r != 0)
    {
        if (aconf.error == NULL || strcmp(aconf.error, "already initialized") != 0)
        {
            HLOGE("audio-play", "Error in audio->init(): %s", aconf.error ? aconf.error : "unknown");
            goto finish_play;
        }
        // "already initialized" is fine - another thread owns the context
    }
    else
    {
        did_init_play = true;
    }

#if defined(_WIN32)
    if (conf.buf.device_id)
    {
        const char *requested_device = conf.buf.device_id;
        int resolved = resolve_windows_audio_device_id(audio, FFAUDIO_DEV_PLAYBACK,
                                                       requested_device,
                                                       windows_device_id,
                                                       sizeof(windows_device_id));
        if (resolved == 0)
        {
            conf.buf.device_id = windows_device_id;
            if (strcmp(requested_device, windows_device_id) != 0)
                HLOGI("audio-play", "Resolved Windows playback device '%s' -> '%s'",
                      requested_device, windows_device_id);
        }
        else if (resolved == 1)
        {
            HLOGI("audio-play", "Resolved Windows playback device '%s' -> default",
                  requested_device);
            conf.buf.device_id = NULL;
        }
        else if (audio_subsystem == AUDIO_SUBSYSTEM_DSOUND &&
                 requested_device[0] != '{')
        {
            HLOGE("audio-play", "DirectSound playback device '%s' was not found; using default",
                  requested_device);
            conf.buf.device_id = NULL;
        }

        if (audio_subsystem == AUDIO_SUBSYSTEM_DSOUND && conf.buf.device_id)
        {
            if (conf.buf.device_id[0] == '{' && str_to_guid(conf.buf.device_id, &play_guid) == 0)
                conf.buf.device_id = (const char *)&play_guid;
            else
            {
                HLOGE("audio-play", "Invalid DirectSound playback device '%s'; using default",
                      requested_device);
                conf.buf.device_id = NULL;
            }
        }
    }
#endif

#if defined(__APPLE__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_COREAUDIO && conf.buf.device_id &&
        coreaudio_resolve_device_id(audio, FFAUDIO_DEV_PLAYBACK, conf.buf.device_id, &coreaudio_device_id) == 0)
        conf.buf.device_id = (const char *)&coreaudio_device_id;
#endif

    // playback code...
    b = audio->alloc();
    if (b == NULL)
    {
        HLOGE("audio-play", "Error in audio->alloc()");
        goto finish_play;
    }

    cfg = &conf.buf;
    r = audio->open(b, cfg, conf.flags);
    if (r == FFAUDIO_EFORMAT)
        r = audio->open(b, cfg, conf.flags);
    if (r != 0)
    {
        HLOGE("audio-play", "error in audio->open(): %d: %s", r, audio->error(b));
        goto cleanup_play;
    }

    HLOGI("audio-play", "I/O playback (%s) %s / %dHz / %dch / %dms buffer",
          device_ptr ? (const char *)device_ptr : "default",
          audioio_format_name(cfg->format),
          cfg->sample_rate, cfg->channels, cfg->buffer_length_msec);

    if (!audioio_playback_format_supported(cfg->format))
    {
        HLOGE("audio-play", "Unsupported playback format %d (%s), aborting",
              cfg->format, audioio_format_name(cfg->format));
        goto cleanup_play;
    }

    frame_size = cfg->channels * (cfg->format & 0xff) / 8;
    if (frame_size == 0 || cfg->channels == 0)
    {
        HLOGE("audio-play", "Invalid playback format/channels: format=%d channels=%d",
              cfg->format, cfg->channels);
        goto cleanup_play;
    }

    msec_bytes = cfg->sample_rate * frame_size / 1000;
    resample_ratio = audioio_rate_ratio(cfg->sample_rate, "audio-play");
    if (resample_ratio == 0)
        goto cleanup_play;

    HLOGI("audio-play", "Resampler: %d Hz modem audio -> %d Hz device audio (x%d)",
          AUDIOIO_MODEM_SAMPLE_RATE, cfg->sample_rate, resample_ratio);

#if 0 // TODO: parametrize this
    if (radio_type == RADIO_SBITX)
        ch_layout = RIGHT;
    if (radio_type == RADIO_STOCKHF)
        ch_layout = STEREO;
#endif
    ch_layout = STEREO;
    
    uint32_t period_samples_8k = AUDIOIO_MODEM_SAMPLE_RATE * period_ms / 1000;
    if (period_samples_8k == 0)
        period_samples_8k = 1;
    uint32_t period_bytes_8k = period_samples_8k * sizeof(int32_t);
    size_t max_upsampled_samples = (size_t)period_samples_8k * (size_t)resample_ratio;
    size_t output_bytes = max_upsampled_samples * frame_size;

    input_buffer = (int32_t *)malloc(period_bytes_8k);
    buffer_upsampled = (int32_t *)malloc(max_upsampled_samples * sizeof(int32_t));
    buffer_output = (uint8_t *)malloc(output_bytes);
    if (!input_buffer || !buffer_upsampled || !buffer_output)
    {
        HLOGE("audio-play", "Failed to allocate playback conversion buffers");
        goto cleanup_play;
    }

    while (!shutdown_ && !audio_shutdown_)
    {
        ffssize n;
        size_t buffer_size = size_buffer(playback_buffer);
        if (buffer_size == 0)
        {
            ffthread_sleep(period_ms ? period_ms : 5);
            continue;
        }
        if (buffer_size >= period_bytes_8k)
        {
            read_buffer(playback_buffer, (uint8_t *) input_buffer, period_bytes_8k);
            n = period_bytes_8k;
        }
        else
        {
            // we just play zeros if there is nothing to play
            memset(input_buffer, 0, period_bytes_8k);
            if (buffer_size > 0)
                read_buffer(playback_buffer, (uint8_t *) input_buffer, buffer_size);
            n = buffer_size;
        }

        total_written = 0;

        int samples_read_8k = n / sizeof(int32_t);

        // Upsample from modem rate to the actual device rate using linear interpolation.
        int samples_upsampled = samples_read_8k * resample_ratio;
        for (int i = 0; i < samples_read_8k; i++)
        {
            int32_t current = input_buffer[i];
            int32_t next = (i + 1 < samples_read_8k) ? input_buffer[i + 1] : current;

            for (int j = 0; j < resample_ratio; j++)
            {
                // Linear interpolation between current and next sample
                int64_t delta = (int64_t)next - (int64_t)current;
                buffer_upsampled[i * resample_ratio + j] =
                    (int32_t)((int64_t)current + (delta * j) / resample_ratio);
            }
        }

        // Convert upsampled mono int32 samples to the device's channel count and sample format.
        const int sample_bytes = (cfg->format & 0xff) / 8;
        for (int i = 0; i < samples_upsampled; i++)
        {
            uint8_t *frame = buffer_output + ((size_t)i * frame_size);
            for (unsigned ch = 0; ch < cfg->channels; ch++)
            {
                int32_t out_sample = 0;
                if (ch_layout == STEREO ||
                    (ch_layout == LEFT && ch == 0) ||
                    (ch_layout == RIGHT && ch == 1))
                {
                    out_sample = audioio_apply_playback_gain(buffer_upsampled[i]);
                }
                audioio_store_playback_sample(frame + ((size_t)ch * sample_bytes),
                                              cfg->format, out_sample);
            }
        }

        n = samples_upsampled * frame_size;

        while (n >= frame_size)
        {
            if (audio_shutdown_) break;  // exit fast on restart

            r = audio->write(b, buffer_output + total_written, n);

            if (r == -FFAUDIO_ESYNC) {
                HLOGW("audio-play", "detected underrun");
                continue;
            }
            if (r < 0)
            {
                HLOGE("audio-play", "ffaudio.write: %s", audio->error(b));
            }
#if 0 // print time measurement
            else
            {
                printf(" %dms\n", r / msec_bytes);
            }
#endif
            total_written += r;
            n -= r;
        }
        // printf("n = %lld total written = %u\n", n, total_written);
    }
    // Only drain when doing a full shutdown, not a restart
    // audio->drain() blocks until all buffered data is played out
    // which can hang indefinitely during a device switch
    if (!audio_shutdown_) {
        r = audio->drain(b);
        if (r < 0)
            HLOGE("audio-play", "ffaudio.drain: %s", audio->error(b));
    }
    r = audio->stop(b);
    if (r != 0)
        HLOGE("audio-play", "ffaudio.stop: %s", audio->error(b));

    r = audio->clear(b);
    if (r != 0)
        HLOGE("audio-play", "ffaudio.clear: %s", audio->error(b));

cleanup_play:

    audio->free(b);

    // Only uninit if this thread was the one that initialized the PA context
    if (did_init_play)
        audio->uninit();

finish_play:

    free(input_buffer);
    free(buffer_upsampled);
    free(buffer_output);

    HLOGI("audio-play", "radio_playback_thread exit");

    // Only trigger global shutdown if this was NOT a restart-initiated stop
    if (!audio_shutdown_)
        shutdown_ = true;

    return NULL;
}


void *radio_capture_thread(void *device_ptr)
{
    ffaudio_interface *audio = NULL;
    struct conf conf = {};
    int coreaudio_device_id = -1;
#if defined(_WIN32)
    char windows_device_id[2048];
    GUID cap_guid;
#endif
    conf.buf.app_name = "mercury_capture";
    conf.buf.format = FFAUDIO_F_INT32;
    conf.buf.sample_rate = 48000;
    conf.buf.channels = 2;
    conf.buf.device_id = (device_ptr && ((const char *)device_ptr)[0] != '\0')
                         ? (const char *) device_ptr : NULL;

#if defined(_WIN32)
    if (audio_subsystem == AUDIO_SUBSYSTEM_WASAPI) {
        conf.buf.buffer_length_msec = 40;
        audio = (ffaudio_interface *) &ffwasapi;
    }
    if (audio_subsystem == AUDIO_SUBSYSTEM_DSOUND) {
        /* DSound on Win10/11 is emulated via WASAPI. A small looping buffer
         * causes the write cursor to lap our read position between polls,
         * losing most captured data.  Use 500ms (DSound's own default). */
        conf.buf.buffer_length_msec = 200;
        audio = (ffaudio_interface *) &ffdsound;
    }
#elif defined(__linux__)
    conf.buf.buffer_length_msec = 30;
    if (audio_subsystem == AUDIO_SUBSYSTEM_ALSA)
        audio = (ffaudio_interface *) &ffalsa;
    if (audio_subsystem == AUDIO_SUBSYSTEM_PULSE)
        audio = (ffaudio_interface *) &ffpulse;
#elif defined(__FREEBSD__)
    conf.buf.buffer_length_msec = 40;
    if (audio_subsystem == AUDIO_SUBSYSTEM_OSS)
        audio = (ffaudio_interface *) &ffoss;
#elif defined(__APPLE__)
    conf.buf.buffer_length_msec = 200;
    if (audio_subsystem == AUDIO_SUBSYSTEM_COREAUDIO)
        audio = (ffaudio_interface *) &ffcoreaudio;
#endif

    if (!audio)
    {
        HLOGE("audio-cap", "Unsupported audio subsystem: %d", audio_subsystem);
        return NULL;
    }

    conf.flags = FFAUDIO_CAPTURE;
    ffaudio_init_conf aconf = {};
    aconf.app_name = "mercury_capture";

    int r;
    ffaudio_buf *b;
    ffaudio_conf *cfg;

    ffuint frame_size;
    ffuint msec_bytes;

    int32_t *buffer = NULL;

    int ch_layout = STEREO;

    int32_t *buffer_downsampled = NULL;

    int resample_ratio = 0;

    /* PulseAudio uses a single global context (gconn in pulse.c).
     * If init() returns "already initialized" it means the playback thread
     * already called init() successfully and we can proceed normally.
     * Track whether we initialized so we only uninit once.
     */
    bool did_init_cap = false;
    r = audio->init(&aconf);
    if (r != 0)
    {
        if (aconf.error == NULL || strcmp(aconf.error, "already initialized") != 0)
        {
            HLOGE("audio-cap", "Error in audio->init(): %s", aconf.error ? aconf.error : "unknown");
            goto finish_cap;
        }
        // "already initialized" is fine - another thread owns the context
    }
    else
    {
        did_init_cap = true;
    }

#if defined(_WIN32)
    if (conf.buf.device_id)
    {
        const char *requested_device = conf.buf.device_id;
        int resolved = resolve_windows_audio_device_id(audio, FFAUDIO_DEV_CAPTURE,
                                                       requested_device,
                                                       windows_device_id,
                                                       sizeof(windows_device_id));
        if (resolved == 0)
        {
            conf.buf.device_id = windows_device_id;
            if (strcmp(requested_device, windows_device_id) != 0)
                HLOGI("audio-cap", "Resolved Windows capture device '%s' -> '%s'",
                      requested_device, windows_device_id);
        }
        else if (resolved == 1)
        {
            HLOGI("audio-cap", "Resolved Windows capture device '%s' -> default",
                  requested_device);
            conf.buf.device_id = NULL;
        }
        else if (audio_subsystem == AUDIO_SUBSYSTEM_DSOUND &&
                 requested_device[0] != '{')
        {
            HLOGE("audio-cap", "DirectSound capture device '%s' was not found; using default",
                  requested_device);
            conf.buf.device_id = NULL;
        }

        if (audio_subsystem == AUDIO_SUBSYSTEM_DSOUND && conf.buf.device_id)
        {
            if (conf.buf.device_id[0] == '{' && str_to_guid(conf.buf.device_id, &cap_guid) == 0)
                conf.buf.device_id = (const char *)&cap_guid;
            else
            {
                HLOGE("audio-cap", "Invalid DirectSound capture device '%s'; using default",
                      requested_device);
                conf.buf.device_id = NULL;
            }
        }
    }
#endif

#if defined(__APPLE__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_COREAUDIO && conf.buf.device_id &&
        coreaudio_resolve_device_id(audio, FFAUDIO_DEV_CAPTURE, conf.buf.device_id, &coreaudio_device_id) == 0)
        conf.buf.device_id = (const char *)&coreaudio_device_id;
#endif

    // capture code
    b = audio->alloc();
    if (b == NULL)
    {
        HLOGE("audio-cap", "Error in audio->alloc()");
        goto finish_cap;
    }

    cfg = &conf.buf;
    r = audio->open(b, cfg, conf.flags);
    if (r == FFAUDIO_EFORMAT)
        r = audio->open(b, cfg, conf.flags);
    if (r != 0)
    {
        HLOGE("audio-cap", "error in audio->open(): %d: %s", r, audio->error(b));
        goto cleanup_cap;
    }

    HLOGI("audio-cap", "I/O capture (%s) %s / %dHz / %dch / %dms buffer",
          device_ptr ? (const char *)device_ptr : "default",
          audioio_format_name(cfg->format),
          cfg->sample_rate, cfg->channels, cfg->buffer_length_msec);

    frame_size = cfg->channels * (cfg->format & 0xff) / 8;
    if (frame_size == 0 || cfg->channels == 0)
    {
        HLOGE("audio-cap", "Invalid capture format/channels: format=%d channels=%d",
              cfg->format, cfg->channels);
        goto cleanup_cap;
    }

    msec_bytes = cfg->sample_rate * frame_size / 1000;
    resample_ratio = audioio_rate_ratio(cfg->sample_rate, "audio-cap");
    if (resample_ratio == 0)
        goto cleanup_cap;

    HLOGI("audio-cap", "Resampler: %d Hz device audio -> %d Hz modem audio (/%d)",
          cfg->sample_rate, AUDIOIO_MODEM_SAMPLE_RATE, resample_ratio);

    bool capture_is_float = (cfg->format == FFAUDIO_F_FLOAT32);
    bool capture_is_int16 = (cfg->format == FFAUDIO_F_INT16);
    int  capture_channels = cfg->channels;

    if (!capture_is_float && !capture_is_int16 &&
        cfg->format != FFAUDIO_F_INT32 && cfg->format != FFAUDIO_F_INT24_4)
    {
        HLOGE("audio-cap", "Unsupported capture format %d, aborting", cfg->format);
        goto cleanup_cap;
    }

    buffer_downsampled = (int32_t *) malloc(SIGNAL_BUFFER_SIZE * sizeof(int32_t));
    if (!buffer_downsampled)
    {
        HLOGE("audio-cap", "Failed to allocate capture conversion buffer");
        goto cleanup_cap;
    }

#if 0 // TODO: parametrize this
    if (radio_type == RADIO_SBITX)
        ch_layout = LEFT;
    if (radio_type == RADIO_STOCKHF)
        ch_layout = STEREO;
#endif
    ch_layout = capture_input_channel_layout;

    int resample_remainder = 0;  // Track position in the integer downsample cycle.

    /* --- Capture rate diagnostics (prints every ~5 seconds) --- */
    uint64_t diag_start_ms = audioio_monotonic_ms();
    uint64_t diag_total_device_frames = 0;   /* frames read from audio device */
    uint64_t diag_total_8k_samples = 0;   /* samples after downsampling (8kHz) */
    uint32_t diag_read_calls = 0;
    uint32_t diag_read_errors = 0;
    uint32_t diag_buf_full_drops = 0;
    int      diag_last_read_bytes = 0;

    while (!shutdown_ && !audio_shutdown_)
    {
        r = audio->read(b, (const void **)&buffer);
        if (r < 0)
        {
            diag_read_errors++;
            HLOGE("audio-cap", "ffaudio.read: %s", audio->error(b));
            continue;
        }

        diag_read_calls++;
        diag_last_read_bytes = r;

        int frames_read = r / frame_size;
        int frames_to_write = frames_read;
        
        // Downsample from the actual device rate to the modem rate with decimation.
        // resample_remainder tracks position in decimation cycle (0 to resample_ratio-1)
        // When remainder is 0, we take a sample; otherwise skip
        int downsampled_frames = 0;
        for (int i = 0; i < frames_to_write; i++)
        {
            int32_t sample;

            if (capture_channels == 1)
            {
                // Mono: one sample per frame
                if (capture_is_float)
                {
                    float fsample = ((float *)buffer)[i];
                    if (fsample > 1.0f) fsample = 1.0f;
                    else if (fsample < -1.0f) fsample = -1.0f;
                    sample = (int32_t)(fsample * 2147483647.0f);
                }
                else if (capture_is_int16)
                {
                    sample = (int32_t)((int16_t *)buffer)[i] * 65536;
                }
                else
                {
                    sample = buffer[i];
                }
            }
            else
            {
                // Stereo: two samples per frame, extract based on ch_layout
                if (capture_is_float)
                {
                    float *fbuf = (float *)buffer;
                    int base = i * capture_channels;
                    float fl = fbuf[base];
                    float fr = fbuf[base + (capture_channels > 1 ? 1 : 0)];
                    float fs;
                    if (ch_layout == LEFT)
                        fs = fl;
                    else if (ch_layout == RIGHT)
                        fs = fr;
                    else
                        fs = (fl + fr) * 0.5f;
                    if (fs > 1.0f) fs = 1.0f;
                    else if (fs < -1.0f) fs = -1.0f;
                    sample = (int32_t)(fs * 2147483647.0f);
                }
                else if (capture_is_int16)
                {
                    int16_t *i16buf = (int16_t *)buffer;
                    int base = i * capture_channels;
                    if (ch_layout == LEFT)
                        sample = (int32_t)i16buf[base] * 65536;
                    else if (ch_layout == RIGHT)
                        sample = (int32_t)i16buf[base + (capture_channels > 1 ? 1 : 0)] * 65536;
                    else
                        sample = ((int32_t)i16buf[base] +
                                  (int32_t)i16buf[base + (capture_channels > 1 ? 1 : 0)]) * 32768;
                }
                else
                {
                    int base = i * capture_channels;
                    if (ch_layout == LEFT)
                        sample = buffer[base];
                    else if (ch_layout == RIGHT)
                        sample = buffer[base + (capture_channels > 1 ? 1 : 0)];
                    else
                        sample = (int32_t)(((int64_t)buffer[base] +
                                            (int64_t)buffer[base + (capture_channels > 1 ? 1 : 0)]) / 2);
                }
            }

            // Take every Nth sample (when remainder == 0)
            // Bounds check: ensure we don't overflow buffer_downsampled
            if (resample_remainder == 0 && downsampled_frames < (int)SIGNAL_BUFFER_SIZE)
            {
                buffer_downsampled[downsampled_frames++] = sample;
            }

            resample_remainder = (resample_remainder + 1) % resample_ratio;
        }

        if (downsampled_frames > 0)
        {
            if (circular_buf_free_size(capture_buffer) >= (size_t)(downsampled_frames * sizeof(int32_t)))
                write_buffer(capture_buffer, (uint8_t *)buffer_downsampled, downsampled_frames * sizeof(int32_t));
            else
            {
                diag_buf_full_drops += downsampled_frames;
                HLOGW("audio-cap", "Buffer full in capture buffer!");
            }
        }

        diag_total_device_frames += frames_read;
        diag_total_8k_samples += downsampled_frames;

        /* Print diagnostics every ~5 seconds */
        uint64_t diag_now = audioio_monotonic_ms();
        uint64_t diag_elapsed = diag_now - diag_start_ms;
        if (diag_elapsed >= 5000)
        {
#ifdef DEBUG_IO
            double elapsed_sec = diag_elapsed / 1000.0;
            double rate_device = diag_total_device_frames / elapsed_sec;
            double rate_8k  = diag_total_8k_samples / elapsed_sec;
            size_t buf_used = size_buffer(capture_buffer);
            size_t buf_free = circular_buf_free_size(capture_buffer);
            HLOGD("audio-cap",
                  "DIAG: %.1fs | reads=%u errs=%u | device=%.0f Hz (expect %d) | modem=%.0f Hz (expect %d) | last_read=%d B | ringbuf used=%zu free=%zu | drops=%u",
                  elapsed_sec, diag_read_calls, diag_read_errors,
                  rate_device, cfg->sample_rate, rate_8k, AUDIOIO_MODEM_SAMPLE_RATE, diag_last_read_bytes,
                  buf_used, buf_free, diag_buf_full_drops);
#endif /* DEBUG_IO */
            /* reset counters */
            diag_start_ms = diag_now;
            diag_total_device_frames = 0;
            diag_total_8k_samples = 0;
            diag_read_calls = 0;
            diag_read_errors = 0;
            diag_buf_full_drops = 0;
        }
    }

    r = audio->stop(b);
    if (r != 0)
        HLOGE("audio-cap", "ffaudio.stop: %s", audio->error(b));

    r = audio->clear(b);
    if (r != 0)
        HLOGE("audio-cap", "ffaudio.clear: %s", audio->error(b));

    free(buffer_downsampled);

cleanup_cap:

    audio->free(b);

    // Only uninit if this thread was the one that initialized the PA context
    if (did_init_cap)
        audio->uninit();

finish_cap:
    HLOGI("audio-cap", "radio_capture_thread exit");

    // Only trigger global shutdown if this was NOT a restart-initiated stop
    if (!audio_shutdown_)
        shutdown_ = true;

    return NULL;
}

int get_soundcard_list(int audio_system, int mode,
                       char ids[][64], char dev_names[][64], int max_count)
{
    ffaudio_interface *audio = NULL;
    int count = 0;
    bool did_init = false;

    if (audio_system == AUDIO_SUBSYSTEM_SHM)
        return 0;

#if defined(_WIN32)
    if (audio_system == AUDIO_SUBSYSTEM_WASAPI)
        audio = (ffaudio_interface *) &ffwasapi;
    if (audio_system == AUDIO_SUBSYSTEM_DSOUND)
        audio = (ffaudio_interface *) &ffdsound;
#elif defined(__linux__)
    if (audio_system == AUDIO_SUBSYSTEM_ALSA)
        audio = (ffaudio_interface *) &ffalsa;
    if (audio_system == AUDIO_SUBSYSTEM_PULSE)
        audio = (ffaudio_interface *) &ffpulse;
#elif defined(__FREEBSD__)
    if (audio_system == AUDIO_SUBSYSTEM_OSS)
        audio = (ffaudio_interface *) &ffoss;
#elif defined(__APPLE__)
    if (audio_system == AUDIO_SUBSYSTEM_COREAUDIO)
        audio = (ffaudio_interface *) &ffcoreaudio;
#elif defined(__ANDROID__)
    if (audio_system == AUDIO_SUBSYSTEM_AAUDIO)
        audio = (ffaudio_interface *) &ffaaudio;
#endif

    if (!audio)
        return 0;

#if defined(__linux__)
    if (audio_system == AUDIO_SUBSYSTEM_PULSE)
    {
        if (pulse_shared_init(&did_init) != 0)
            return 0;
    }
    else
#endif
    {
        ffaudio_init_conf aconf = {};
        if (audio->init(&aconf) != 0)
            return 0;
        did_init = true;
    }

    // mode: FFAUDIO_DEV_PLAYBACK (0) or FFAUDIO_DEV_CAPTURE (1)
    ffaudio_dev *d = audio->dev_alloc(mode);
    if (d == NULL)
    {
        if (did_init)
            audio->uninit();
        return 0;
    }

    for (;;)
    {
        int r = audio->dev_next(d);
        if (r != 0)
            break;
        const char *id = audio->dev_info(d, FFAUDIO_DEV_ID);
        const char *name = audio->dev_info(d, FFAUDIO_DEV_NAME);
        if (id && count < max_count)
        {
            strncpy(ids[count], id, 63);
            ids[count][63] = '\0';
            if (name) {
                strncpy(dev_names[count], name, 63);
                dev_names[count][63] = '\0';
            } else {
                strncpy(dev_names[count], id, 63);
                dev_names[count][63] = '\0';
            }
            count++;
        }
    }

    audio->dev_free(d);
    if (did_init)
        audio->uninit();
    return count;
}

void list_soundcards(int audio_system)
{
    ffaudio_interface *audio = NULL;
    bool did_init = false;
    audio_subsystem = audio_system;

    if (audio_subsystem == AUDIO_SUBSYSTEM_SHM)
    {
        // TODO: connect to the shared memory
        printf("Shared Memory (SHM) audio subsystem selected.\n");
        audio = NULL;
        return;
    }
    
#if defined(_WIN32)
    if (audio_subsystem == AUDIO_SUBSYSTEM_WASAPI)
        audio = (ffaudio_interface *) &ffwasapi;
    if (audio_subsystem == AUDIO_SUBSYSTEM_DSOUND)
        audio = (ffaudio_interface *) &ffdsound;
#elif defined(__linux__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_ALSA)
    {
        printf("Listing ALSA soundcards:\n");
        audio = (ffaudio_interface *) &ffalsa;
    }
    if (audio_subsystem == AUDIO_SUBSYSTEM_PULSE)
        audio = (ffaudio_interface *) &ffpulse;
#elif defined(__FREEBSD__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_OSS)
        audio = (ffaudio_interface *) &ffoss;
#elif defined(__APPLE__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_COREAUDIO)
        audio = (ffaudio_interface *) &ffcoreaudio;
#elif defined(__ANDROID__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_AAUDIO)
        audio = (ffaudio_interface *) &ffaaudio;
#endif

    if (!audio)
        return;

#if defined(__linux__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_PULSE)
    {
        if (pulse_shared_init(&did_init) != 0)
        {
            printf("Error in audio->init()\n");
            return;
        }
    }
    else
#endif
    {
        ffaudio_init_conf aconf = {};
        if (audio->init(&aconf) != 0)
        {
            printf("Error in audio->init()\n");
            return;
        }
        did_init = true;
    }

    ffaudio_dev *d;

    // FFAUDIO_DEV_PLAYBACK, FFAUDIO_DEV_CAPTURE
    static const char* const mode[] = { "playback", "capture" };
    for (ffuint i = 0;  i != 2;  i++)
    {
        printf("%s devices:\n", mode[i]);
        d = audio->dev_alloc(i);
        if (d == NULL)
        {
            printf("Error in audio->dev_alloc\n");
            if (did_init)
                audio->uninit();
            return;
        }

        for (;;)
        {
            int r = audio->dev_next(d);
            if (r > 0)
                break;
            else
                if (r < 0)
                {
                    printf("error: %s", audio->dev_error(d));
                    break;
                }

            const char *id = audio->dev_info(d, FFAUDIO_DEV_ID);
#if defined(__APPLE__)
            char id_buf[32] = {0};
            if (audio_system == AUDIO_SUBSYSTEM_COREAUDIO && id)
            {
                int numeric_id = -1;
                memcpy(&numeric_id, id, sizeof(numeric_id));
                snprintf(id_buf, sizeof(id_buf), "%d", numeric_id);
                id = id_buf;
            }
#endif
            printf("device: name: '%s'  id: '%s'  default: %s\n"
                   , audio->dev_info(d, FFAUDIO_DEV_NAME)
                   , id ? id : ""
                   , audio->dev_info(d, FFAUDIO_DEV_IS_DEFAULT)
                );
        }

        audio->dev_free(d);
    }

    if (did_init)
        audio->uninit();
}

#if 0
// size in "double" samples
int tx_transfer(double *buffer, size_t len)
{
    uint8_t *buffer_internal = (uint8_t *) buffer;
    int buffer_size_bytes = len * sizeof(double);

    write_buffer(playback_buffer, buffer_internal, buffer_size_bytes);

    // printf("size %llu free %llu\n", size_buffer(playback_buffer), circular_buf_free_size(playback_buffer));

    return 0;
}

// size in "double" samples
int rx_transfer(double *buffer, size_t len)
{
    uint8_t *buffer_internal = (uint8_t *) buffer;
    int buffer_size_bytes = len * sizeof(double);

    read_buffer(capture_buffer, buffer_internal, buffer_size_bytes);

    return 0;
}
#endif

int audioio_init_buffers(void)
{
    if (s_buffers_initialized)
        return 0;  // already created

    uint8_t *buffer_cap = (uint8_t *)malloc(SIGNAL_BUFFER_SIZE);
    uint8_t *buffer_play = (uint8_t *)malloc(SIGNAL_BUFFER_SIZE);
    if (!buffer_cap || !buffer_play)
    {
        free(buffer_cap);
        free(buffer_play);
        HLOGE("audioio", "Failed to allocate local audio buffers");
        return -1;
    }
    capture_buffer = circular_buf_init(buffer_cap, SIGNAL_BUFFER_SIZE);
    playback_buffer = circular_buf_init(buffer_play, SIGNAL_BUFFER_SIZE);
    s_buffers_are_shm = 0;

    clear_buffer(capture_buffer);
    clear_buffer(playback_buffer);
    s_buffers_initialized = 1;
    return 0;
}

void audioio_deinit_buffers(void)
{
    if (!s_buffers_initialized)
        return;

    if (s_buffers_are_shm)
    {
        circular_buf_destroy_shm(capture_buffer, SIGNAL_BUFFER_SIZE, (char *) SIGNAL_INPUT);
        circular_buf_free_shm(capture_buffer);

        circular_buf_destroy_shm(playback_buffer, SIGNAL_BUFFER_SIZE, (char *) SIGNAL_OUTPUT);
        circular_buf_free_shm(playback_buffer);
    }
    else
    {
        free(capture_buffer->buffer);
        circular_buf_free(capture_buffer);
        free(playback_buffer->buffer);
        circular_buf_free(playback_buffer);
    }

    capture_buffer = NULL;
    playback_buffer = NULL;
    s_buffers_are_shm = 0;
    s_buffers_initialized = 0;
}

int audioio_init_internal(char *capture_dev, char *playback_dev, int audio_subsys, int capture_channel_layout, pthread_t *radio_capture,
                          pthread_t *radio_playback)
{
    audio_subsystem = audio_subsys;
    if (capture_channel_layout == LEFT ||
        capture_channel_layout == RIGHT ||
        capture_channel_layout == STEREO)
        capture_input_channel_layout = capture_channel_layout;
    else
        capture_input_channel_layout = LEFT;

    // Store device names for restart support
    if (capture_dev)
    {
        strncpy(s_capture_dev, capture_dev, sizeof(s_capture_dev) - 1);
        s_capture_dev[sizeof(s_capture_dev) - 1] = '\0';
    }
    else
        s_capture_dev[0] = '\0';
    if (playback_dev)
    {
        strncpy(s_playback_dev, playback_dev, sizeof(s_playback_dev) - 1);
        s_playback_dev[sizeof(s_playback_dev) - 1] = '\0';
    }
    else
        s_playback_dev[0] = '\0';

    // Create process-local buffers for the internal sound-card path.
    if (audioio_init_buffers() != 0)
        return -1;

    /* Pre-initialize PulseAudio once here in the main thread before spawning
     * capture/playback threads. ffpulse_init() uses a single global context
     * (gconn) and returns an error if called more than once. By initializing
     * here, both threads will see "already initialized" and proceed normally
     * rather than one of them failing and exiting early.
     */
#if defined(__linux__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_PULSE)
    {
        (void) pulse_shared_init(NULL);
    }
#endif

    pthread_create(radio_capture, NULL, radio_capture_thread, (void *) s_capture_dev);
    pthread_create(radio_playback, NULL, radio_playback_thread, (void *) s_playback_dev);

    // Keep internal copies of thread handles
    s_radio_capture = *radio_capture;
    s_radio_playback = *radio_playback;

    return 0;
}

static void audioio_stop_threads(void)
{
    // Signal audio threads to exit their loops
    audio_shutdown_ = true;
    pthread_join(s_radio_capture, NULL);
    pthread_join(s_radio_playback, NULL);
    audio_shutdown_ = false;

#if defined(__linux__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_PULSE)
        pulse_shared_uninit();
#endif

    HLOGI("audio-stop", "audioio threads stopped");
}

int audioio_restart(const char *capture_dev, const char *playback_dev,
                    int audio_subsys, int capture_channel_layout)
{
    HLOGI("audio-restart", "stopping audio threads...");
    audioio_stop_threads();

    // Update stored parameters
    audio_subsystem = audio_subsys;
    if (capture_channel_layout == LEFT ||
        capture_channel_layout == RIGHT ||
        capture_channel_layout == STEREO)
        capture_input_channel_layout = capture_channel_layout;
    else
        capture_input_channel_layout = LEFT;

    if (capture_dev && capture_dev[0] != '\0')
    {
        strncpy(s_capture_dev, capture_dev, sizeof(s_capture_dev) - 1);
        s_capture_dev[sizeof(s_capture_dev) - 1] = '\0';
    }

    if (playback_dev && playback_dev[0] != '\0')
    {
        strncpy(s_playback_dev, playback_dev, sizeof(s_playback_dev) - 1);
        s_playback_dev[sizeof(s_playback_dev) - 1] = '\0';
    }

    // Clear buffers (NEVER destroy/recreate them)
    clear_buffer(capture_buffer);
    clear_buffer(playback_buffer);

    HLOGI("audio-restart", "starting audio threads (capture=%s playback=%s channel=%d)...",
           s_capture_dev[0] ? s_capture_dev : "default",
           s_playback_dev[0] ? s_playback_dev : "default",
           capture_input_channel_layout);

#if defined(__linux__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_PULSE)
    {
        (void) pulse_shared_init(NULL);
    }
#endif

    pthread_create(&s_radio_capture, NULL, radio_capture_thread, (void *) s_capture_dev);
    pthread_create(&s_radio_playback, NULL, radio_playback_thread, (void *) s_playback_dev);

    HLOGI("audio-restart", "audio threads restarted");
    return 0;
}

int audioio_deinit(pthread_t *radio_capture, pthread_t *radio_playback)
{
    // The external thread handles may be stale after a restart; use internal statics instead.
    (void) radio_capture;
    (void) radio_playback;
    pthread_join(s_radio_capture, NULL);
    pthread_join(s_radio_playback, NULL);

#if defined(__linux__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_PULSE)
        pulse_shared_uninit();
#endif

    audioio_deinit_buffers();
    return 0;
}

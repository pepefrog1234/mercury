# Mercury Fork — HF Modem

[繁體中文](README.md)

This repository is the `pepefrog1234/mercury` fork of the Mercury HF modem. It is based on Rhizomatica's HERMES Mercury modem and is primarily maintained for Mercury Chat and HF digital communication experiments.

This is not the official Rhizomatica release. When used with Mercury Chat, both peers should use this fork or a compatible build. This fork changes ARQ protocol behavior and TNC features, so interoperability with upstream Mercury or other unmodified builds is not guaranteed.

## Fork Origin

- Upstream project: Rhizomatica Mercury
- Upstream URL: https://github.com/Rhizomatica/mercury
- Upstream branch: `mercuryv2`
- Fork branch: `mercuryv2`
- Current fork point: upstream `mercuryv2` commit `f308d5a`, `Add workflow to automate release process`

Mercury is part of Rhizomatica's HERMES project. Upstream Mercury v2 is a C rewrite of the HF modem with FreeDV DATAC modes, ARQ data link, broadcast/beacon support, a VARA-compatible TCP TNC interface, and Hamlib / HERMES radio control.

## What This Fork Changes

### ARQ Protocol and Link Efficiency

- Introduces ARQ wire protocol v5 with burst DATA and cumulative ACK:
  - DATAC4 remains single-frame.
  - DATAC3 can send up to 2 DATA frames under one PTT.
  - DATAC1 can send up to 3 DATA frames under one PTT.
  - ACKs are cumulative; if a burst loses a frame, only the unacknowledged burst tail is retransmitted.
- Improves half-duplex turn-taking:
  - ACK wait and TURN_REQ timing were adjusted to reduce collisions and premature turn switches.
  - When the IRS has local queued data, it gives the peer room to finish data and ACK handling before requesting the turn.
- Relaxes idle and keepalive behavior:
  - IRS inactivity probing uses a more human-chat-friendly time scale.
  - Keepalive / idle disconnect decisions are more tolerant, reducing disconnects while both peers are still present.
- Adds ARQ duty-cycle and timing telemetry for PTT airtime, ACK wait, and payload-rate analysis.

### Adaptive Rate and Bandwidth Rules

- ARQ payload uses a DATAC4 / DATAC3 / DATAC1 ladder:
  - DATAC4: most conservative, about 87 bps, 54-byte payload.
  - DATAC3: medium speed, about 321 bps, 126-byte payload.
  - DATAC1: fastest, about 980 bps, 510-byte payload.
- DATAC13 is reserved for control frames such as CALL, ACCEPT, ACK, TURN, KEEPALIVE, DISCONNECT, and CQ. It is about 65 bps with a 14-byte payload.
- Wide links no longer stay stuck on DATAC4 indefinitely:
  - Short chat payloads may move to DATAC3 once peer SNR is known.
  - DATAC1 requires clean ACK history plus sufficient SNR / backlog.
  - Retries or unstable delivery force a downgrade and hold the lower mode for a short period to avoid rate flapping.
- 500 Hz / 2300 Hz / 2750 Hz behavior is explicit:
  - `BW500` keeps payload strictly narrow on DATAC4; control and beacon frames stay on DATAC13.
  - `BW2300` and `BW2750` allow DATAC4 → DATAC3 → DATAC1 adaptive switching.
  - `BW2750` is preserved in CALL / ACCEPT / CONNECTED reports for VARA-compatible clients.
- A→B and B→A choose their modes independently, so TX/RX rate shown by each GUI may differ.

### TNC Interface and Status Reporting

- Keeps the VARA-style TCP TNC layout:
  - Control port: default `8300`.
  - Data port: default `8301`.
  - Broadcast/beacon port: default `8100`.
- Adds or strengthens control commands:
  - `TXGAIN <0..200>`: live TX audio output gain percentage, where `100` is unity gain.
  - `CALLINT <seconds>`: override CALL / ACCEPT retry interval; `0` restores the default.
  - `BW2750`: accepted and preserved as a bandwidth token.
  - `BUFFER`, `BITRATE`: queryable status commands.
  - `COMPRESSION ON/OFF`: no-op compatibility command for VARA-style clients.
- Adds asynchronous `TXBITRATE (<level>) <bps> BPS`, allowing clients to display local TX bitrate separately from peer RX bitrate.
- `BITRATE (<level>) <bps> BPS` reports the current received payload-mode bitrate.

## Bitrate Table

These are nominal FreeDV DATAC bitrates. They are not the same as net chat throughput. Actual user-data throughput is reduced by ARQ headers, ACKs, turn-taking, PTT delay, retries, and UTF-8 byte counts.

| TNC level | FreeDV mode | Nominal bitrate | Payload per modem frame | Use |
| --- | --- | ---: | ---: | --- |
| `L1` | DATAC1 | about 980 bps | 510 bytes | Fastest payload mode for high SNR, wide bandwidth, and larger backlog |
| `L3` | DATAC3 | about 321 bps | 126 bytes | Medium-speed payload mode commonly used on wide chat links |
| `L4` | DATAC4 | about 87 bps | 54 bytes | Conservative payload mode for narrow or unstable links |
| Control | DATAC13 | about 65 bps | 14 bytes | CALL, ACCEPT, ACK, TURN, KEEPALIVE, DISCONNECT, CQ |

## Audio and Cross-Platform Fixes

- Fixes macOS CoreAudio device resolution:
  - Friendly device names can be resolved to actual CoreAudio device IDs.
  - This improves A/B loopback setups, BlackHole, AetherSDR, and similar audio devices.
- Fixes audio loopback sample handling to avoid waveform errors caused by sample format and sample-rate handling.
- Adds Windows audio device name resolution:
  - WASAPI can accept friendly device names and resolve them to MMDevice IDs.
  - DirectSound can accept friendly device names and resolve them to GUIDs.
  - This avoids cases where CAT/PTT works but audio is not sent to the selected sound card.
- Adds TX audio output gain control:
  - Command-line option: `-Y <0..200>`.
  - INI setting: `tx_audio_gain_percent`.
  - TNC command: `TXGAIN <0..200>`, allowing live adjustment without restarting the modem.

## Build and Packaging Changes

- Fixes macOS build portability.
- The `Makefile` uses the configured archiver for the FreeDV static library.
- Adds fork packaging workflows:
  - Debian 13.5 amd64 build, test, and package.
  - Windows x64 MinGW cross-build and zip package.
- Windows zip packages include `mercury.exe`, `mercury.ini.example`, and Hamlib-related DLLs.

## Basic Usage

List audio devices:

```sh
./mercury -z
```

List FreeDV modes:

```sh
./mercury -l
```

Start the modem with CoreAudio, selected input/output devices, and a TNC base port:

```sh
./mercury -x coreaudio -i "<input-device>" -o "<output-device>" -p 8300 -b 8100
```

Set TX audio output gain to 80%:

```sh
./mercury -Y 80
```

For all options:

```sh
./mercury -h
```

## Configuration

Mercury reads an INI configuration file. The default path is `mercury.ini` in the current directory; use `-C <path>` to specify another file. Command-line options override configuration-file values.

See:

- [mercury.ini.example](mercury.ini.example)

## Related Documentation

- [ARQ architecture and protocol reference](docs/ARQ.md)
- [TNC command reference](docs/TNC.md)
- [Original Rhizomatica Mercury](https://github.com/Rhizomatica/mercury)
- [Mercury Chat](https://github.com/pepefrog1234/mercury-chat)

## License

This fork keeps the upstream licenses. See:

- [LICENSE](LICENSE)
- [LICENSE-freedv](LICENSE-freedv)

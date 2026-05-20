# ARQ Datalink — Architecture and Protocol Reference

This document covers the ARQ (Automatic Repeat Request) datalink layer introduced
in Mercury v2 (`datalink_arq/`).  It replaces the original monolithic `arq.c` with
a table-driven, two-level hierarchical FSM and a clean protocol codec.

---

## Table of Contents

1. [Overview](#overview)
2. [Module Map](#module-map)
3. [Wire Protocol v5](#wire-protocol-v5)
4. [Frame Types and Subtypes](#frame-types-and-subtypes)
5. [Mode Timing Table](#mode-timing-table)
6. [Two-Level FSM](#two-level-fsm)
   - [Level 1 — Connection FSM](#level-1--connection-fsm)
   - [Level 2 — Data-Flow Sub-FSM](#level-2--data-flow-sub-fsm)
7. [Events](#events)
8. [Session Roles: ISS and IRS](#session-roles-iss-and-irs)
9. [Turn Mechanism](#turn-mechanism)
10. [Mode Upgrade / Downgrade](#mode-upgrade--downgrade)
11. [Keepalive](#keepalive)
12. [Timing Instrumentation](#timing-instrumentation)
13. [Logging](#logging)
14. [Tuning Guide](#tuning-guide)
15. [Source Files](#source-files)

---

## Overview

Mercury ARQ provides a reliable, half-duplex point-to-point data channel over HF
radio using the FreeDV modem stack.

Key properties:
- **Half-duplex**, explicit turn-taking (ISS/IRS roles).
- **DATAC13** is the *control-only* mode: CALL, ACCEPT, ACK, DISCONNECT, TURN_REQ/ACK,
  KEEPALIVE, MODE_REQ/ACK frames are always 14 bytes on DATAC13.
- **Data frames** start in DATAC4 (54 bytes payload) and may upgrade to DATAC3 (126 bytes)
  or DATAC1 (510 bytes) based on SNR and backlog.
- **v5 burst DATA** sends short consecutive DATA runs under one PTT and confirms them
  with one cumulative ACK. DATAC4 remains single-frame; DATAC3 may send 2 frames;
  DATAC1 may send 3 frames.
- **VARA-compatible TCP TNC** interface: control on `base_port` (default 8300), data on
  `base_port+1` (8301). This interface is frozen and not modified by the ARQ rewrite.
- **Broadcast** runs in parallel on a separate TCP port (default 8100) and is
  completely independent of ARQ.

---

## Module Map

```
datalink_arq/
  arq.c           — Public API entry-point, event-loop thread, TCP bridge threads
  arq.h           — Public API (frozen; VARA-compatible)
  arq_fsm.c/h     — Two-level hierarchical FSM (state tables + transitions)
  arq_protocol.c/h— Wire codec: frame builders, parsers, mode timing table
  arq_timing.c/h  — Timing instrumentation and [TMG] log records
  arq_modem.c/h   — Action queue (FSM→modem), PTT event injection (modem→FSM)
  arq_channels.c/h— Channel bus between TCP layer and ARQ event loop
  arq_events.h    — (generated) event forward declarations
  fsm.c/h         — Legacy generic FSM (kept for link compatibility)
  arith.c         — Arithmetic codec for callsign compression
  old_arq/        — Old 4495-line monolith (reference only, not compiled)
```

---

## Wire Protocol v5

All ARQ frames begin with a **framer byte** managed by `modem/framer.c`:

```
Byte 0: [packet_type(3)] | [extension_field(5)]
         bits [7:5] = packet_type
         bits [4:0] = packet-type-specific extension field
```

Packet type values:

| Value | Name                  | Used for                            |
|-------|-----------------------|-------------------------------------|
| 0x0   | `ARQ_CONTROL`         | ACK, DISCONNECT, KEEPALIVE, TURN, MODE |
| 0x1   | `ARQ_DATA`            | Data payload frames                 |
| 0x2   | `ARQ_CALL`            | CALL and ACCEPT compact frames      |
| 0x3   | `BROADCAST_CONTROL`   | Broadcast subsystem (unrelated)     |
| 0x4   | `BROADCAST_DATA`      | Broadcast subsystem (unrelated)     |
| 0x5   | `ARQ_CQ`              | Compact DATAC13 CQ metadata frame   |

### Standard 8-byte header (ARQ_CONTROL and ARQ_DATA)

```
Byte 0: framer byte  (packet_type | extension_field)
Byte 1: subtype      (arq_subtype_t)
Byte 2: flags        bit7=TURN_REQ  bit6=HAS_DATA  bit5=LEN_HI  bit4=BURST_MORE
Byte 3: session_id   random byte chosen by caller at connect time
Byte 4: tx_seq       sender's frame sequence number
Byte 5: rx_ack_seq   cumulative ACK: next sequence number expected from peer
Byte 6: snr_raw      local RX SNR as uint8 = round(snr_dB) + 128; 0=unknown
Byte 7: ack_delay    IRS→ISS delay from data_rx to ack_tx, in 10ms units; 0=unknown
```

For **DATA frames** (`ARQ_DATA`), payload bytes follow immediately after byte 7.
The payload size is determined by the FreeDV mode in use.
For current `ARQ_CONTROL` and `ARQ_DATA` frames, the extension field is transmitted
as `0` and reserved for future use.

DATA-specific flag use:

| Flag | Meaning |
|------|---------|
| `LEN_HI` (`0x20`) | Bit 8 of the valid-payload length carried in byte 7. Required for DATAC1 partial frames. |
| `BURST_MORE` (`0x10`) | More consecutive DATA frames follow before the receiver should ACK. |

### v5 burst and cumulative ACK

Mercury v5 is no longer pure stop-and-wait for payload data. The ISS may send a
short DATA burst in one modem action/PTT, then wait for one ACK from the IRS.
The ACK's `rx_ack_seq` is the next sequence number the IRS expects, so it
cumulatively acknowledges all earlier in-order frames.

Burst limits are intentionally conservative for HF:

| Mode | Max DATA frames per burst | Reason |
|------|---------------------------|--------|
| DATAC4 | 1 | Narrow/robust mode; preserve old stop-and-wait behavior. |
| DATAC3 | 2 | Moderate throughput gain with bounded loss recovery cost. |
| DATAC1 | 3 | High-SNR/high-backlog mode where ACK/turn overhead dominates. |

If a burst frame is lost, the IRS ACKs the first missing sequence number. The ISS
retransmits only the unACKed tail of the saved burst. After retries or unstable
delivery, the ISS falls back to one-frame bursts until the link earns clean ACKs
again.

### CALL/ACCEPT compact frame (ARQ_CALL, 14 bytes)

CALL and ACCEPT use a different layout to fit two callsigns in 14 bytes:

```
Byte 0:      framer byte  (PACKET_TYPE_ARQ_CALL | BW token)
Byte 1:      connect_meta = (session_id & 0x7F) | (is_accept ? 0x80 : 0x00)
Bytes 2-3:   CRC16-CCITT of DST callsign (little-endian) — for local validation
Bytes 4-13:  arithmetic_encode(SRC callsign only) — 10 bytes, fits any realistic callsign
```

The transmitting side's callsign (SRC) is compressed with an arithmetic codec (`arith.c`).
The receiving side's callsign (DST) is not transmitted in full; instead its CRC16 is sent
so the receiver can silently discard frames not addressed to it.

BW token values in the framer-byte extension field:

| Token | Meaning   |
|-------|-----------|
| `0`   | Reserved  |
| `1`   | `BW500`   |
| `2`   | `BW2300`  |
| `3`   | `BW2750`  |

Handshake semantics:

- `CALL` advertises the caller's configured BW token.
- `ACCEPT` returns the negotiated session BW token, computed as `min(caller, callee)`.
- Once `ACCEPT` is processed, both peers know the session bandwidth without adding a
  separate negotiation round trip.
- A negotiated `BW500` session keeps ARQ payload on DATAC4. `BW2300` and
  `BW2750` sessions may use the full DATAC4/DATAC3/DATAC1 ladder.

### CQ compact frame (ARQ_CQ, 14 bytes)

Mercury also defines a compact DATAC13 CQ metadata frame:

```
Byte 0:      framer byte  (PACKET_TYPE_ARQ_CQ | BW token)
Bytes 1-13:  arithmetic_encode(SRC callsign only)
```

The BW token uses the same values as `ARQ_CALL`. When a CQ frame is decoded,
Mercury emits `CQFRAME <source> <bw>` on the TNC control port.

### Integrity note

The framer byte no longer carries a CRC5. Mercury relies on the modem-layer
FreeDV frame integrity checks for whole-frame validation, while `CALL` / `ACCEPT`
still keep the destination callsign CRC16 so receivers can quickly reject connect
attempts not addressed to them.

---

## Frame Types and Subtypes

| Subtype value | Name            | Direction       | Mode used |
|---------------|-----------------|-----------------|-----------|
| 1             | CALL            | Caller → Callee | DATAC13   |
| 2             | ACCEPT          | Callee → Caller | DATAC13   |
| 3             | ACK             | IRS → ISS       | DATAC13   |
| 4             | DISCONNECT      | Either          | DATAC13   |
| 5             | DATA            | ISS → IRS       | DATAC4/3/1|
| 6             | KEEPALIVE       | Either          | DATAC13   |
| 7             | KEEPALIVE_ACK   | Either          | DATAC13   |
| 8             | MODE_REQ        | ISS → IRS       | DATAC13   |
| 9             | MODE_ACK        | IRS → ISS       | DATAC13   |
| 10            | TURN_REQ        | IRS → ISS       | DATAC13   |
| 11            | TURN_ACK        | ISS → IRS       | DATAC13   |

Note: Subtype 12 (`FLOW_HINT`) from the old protocol was removed; the `HAS_DATA`
flag in byte 2 serves the same purpose.

---

## Mode Timing Table

Empirical values from NVIS HF path OTA testing.  All times are seconds.

| Mode    | Payload bytes | Frame duration | TX period | ACK timeout | Retry interval |
|---------|---------------|----------------|-----------|-------------|----------------|
| DATAC13 | 14            | 2.5 s          | 1.0 s     | 6.0 s       | 7.0 s          |
| DATAC4  | 54            | 5.8 s          | 1.0 s     | 12.0 s      | 13.0 s         |
| DATAC3  | 126           | 3.82 s         | 1.0 s     | 9.0 s       | 10.0 s         |
| DATAC1  | 510           | 4.81 s         | 1.0 s     | 11.0 s      | 12.0 s         |

- **ACK timeout**: measured from PTT-OFF after the current frame/burst.
  It must cover DATAC13 ACK return plus guard and any piggybacked DATA response.
- **Retry interval**: `= ack_timeout + ARQ_ACK_GUARD_S (1 s)`.
- **Channel guard**: 700 ms after frame decode before the IRS transmits ACK.
- **ISS post-ACK guard**: 900 ms before the ISS resumes DATA TX after receiving ACK.

These values are defined as constants in `arq_protocol.h` and can be tuned there.

---

## Two-Level FSM

The FSM is entirely in `arq_fsm.c`.  All state is in `arq_session_t`; there are
no hidden global flags.  The FSM is driven by a single event-loop thread via
`arq_fsm_dispatch()`.

### Level 1 — Connection FSM

```
                    EV_APP_LISTEN
  DISCONNECTED ─────────────────► LISTENING
       ▲                              │
       │ (no client /                 │ EV_RX_CALL (to us)
       │  EV_APP_DISCONNECT)          ▼
       │                          ACCEPTING ──── EV_APP_DISCONNECT ──► DISCONNECTED
       │                              │
       │                              │ EV_RX_DATA / EV_RX_ACK (first data)
       │                              │ (sends ACCEPT retries meanwhile)
       │
  DISCONNECTING ◄──────────────── CONNECTED ◄─────────────────────────────────┐
       │                              ▲                                        │
       │ EV_APP_DISCONNECT            │ EV_RX_ACCEPT                          │
       │ or EV_RX_DISCONNECT          │                                        │
       ▼                          CALLING                                      │
  (sends DISCONNECT               │                                            │
   retries, then                  │ EV_APP_CONNECT                             │
   → DISCONNECTED)            DISCONNECTED ────────────────────────────────────┘
                                  (EV_APP_LISTEN → LISTENING)
```

States:

| State          | Description                                        |
|----------------|----------------------------------------------------|
| DISCONNECTED   | No active session; idle                            |
| LISTENING      | Waiting for an incoming CALL frame                 |
| CALLING        | Outgoing CALL sent; awaiting ACCEPT                |
| ACCEPTING      | ACCEPT sent; awaiting first data or ACK            |
| CONNECTED      | Data-flow sub-FSM is active                        |
| DISCONNECTING  | DISCONNECT frame exchange in progress              |

If the caller receives `ACCEPT` before any application payload is queued, it
sends an empty `ACK` after the post-`ACCEPT` guard to complete the handshake.
Without that confirmation, the callee remains in `ACCEPTING` and keeps retrying
`ACCEPT` until its window expires.
If a duplicate `ACCEPT` arrives for the active session while the caller is
already connected and idle, the caller re-sends that confirmation `ACK`.

### Level 2 — Data-Flow Sub-FSM

Active only when L1 is in `CONNECTED`.  Manages the half-duplex turn-taking and
retransmission loop.

```
ISS side:                              IRS side:

IDLE_ISS ──(APP_DATA_READY)──► DATA_TX   IDLE_IRS ──(RX_DATA)──► DATA_RX
              │                   │                                    │
              │ TX_STARTED         │ TX_COMPLETE                        │ (ACK after burst)
              ▼                   ▼                                    ▼
            DATA_TX           WAIT_ACK                              ACK_TX
              │                   │                                    │
              │                   │ RX_ACK → next burst/tail           │ TX_COMPLETE
              │                   │ or retry / TURN_ACK                ▼
              │                   │                              IDLE_IRS (or TURN_REQ_TX
              │                   │                               if HAS_DATA was set)
              │                   │
              │                   └─(timeout)──► retry or disconnect
              │
              └──(peer TURN_REQ) TURN_ACK_TX ──► IDLE_IRS
```

Full state list:

| State            | Description                                    |
|------------------|------------------------------------------------|
| IDLE_ISS         | ISS: no pending frame; waiting for TX data     |
| DATA_TX          | ISS: frame or short burst queued/on air        |
| WAIT_ACK         | ISS: PTT-OFF; waiting for cumulative ACK       |
| IDLE_IRS         | IRS: waiting for peer data frame               |
| DATA_RX          | IRS: data frame/burst decoded; ACK pending     |
| ACK_TX           | IRS: ACK frame being transmitted               |
| TURN_REQ_TX      | IRS→ISS: TURN_REQ being transmitted            |
| TURN_REQ_WAIT    | IRS→ISS: waiting for TURN_ACK                 |
| TURN_ACK_TX      | ISS→IRS: TURN_ACK being transmitted            |
| MODE_REQ_TX      | Mode upgrade: MODE_REQ being transmitted       |
| MODE_REQ_WAIT    | Mode upgrade: waiting for MODE_ACK             |
| MODE_ACK_TX      | Mode upgrade: MODE_ACK being transmitted       |
| KEEPALIVE_TX     | KEEPALIVE being transmitted                    |
| KEEPALIVE_WAIT   | Waiting for KEEPALIVE_ACK                      |

---

## Events

Events are dispatched to the FSM via `arq_fsm_dispatch()`.

### Application events (from TCP TNC via channel bus)

| Event               | Triggered by                     |
|---------------------|----------------------------------|
| `EV_APP_LISTEN`     | `LISTEN ON` command              |
| `EV_APP_STOP_LISTEN`| `LISTEN OFF` command             |
| `EV_APP_CONNECT`    | `CONNECT <dst>` command          |
| `EV_APP_DISCONNECT` | `DISCONNECT` command             |
| `EV_APP_DATA_READY` | Data bytes arrived from TCP      |

### Radio RX events (from modem)

| Event                | Triggered by                     |
|----------------------|----------------------------------|
| `EV_RX_CALL`         | CALL frame decoded               |
| `EV_RX_ACCEPT`       | ACCEPT frame decoded             |
| `EV_RX_ACK`          | ACK frame decoded                |
| `EV_RX_DATA`         | DATA frame decoded               |
| `EV_RX_DISCONNECT`   | DISCONNECT frame decoded         |
| `EV_RX_TURN_REQ`     | TURN_REQ frame decoded           |
| `EV_RX_TURN_ACK`     | TURN_ACK frame decoded           |
| `EV_RX_MODE_REQ`     | MODE_REQ frame decoded           |
| `EV_RX_MODE_ACK`     | MODE_ACK frame decoded           |
| `EV_RX_KEEPALIVE`    | KEEPALIVE frame decoded          |
| `EV_RX_KEEPALIVE_ACK`| KEEPALIVE_ACK frame decoded      |

### Timer events (synthesised by event loop)

| Event                  | Fired when                                     |
|------------------------|------------------------------------------------|
| `EV_TIMER_RETRY`       | Retry deadline expired (ack_timeout + guard)   |
| `EV_TIMER_TIMEOUT`     | Session/call timeout                           |
| `EV_TIMER_ACK`         | ACK wait deadline                              |
| `EV_TIMER_PEER_BACKLOG`| Peer-backlog hold (15 s) expired               |
| `EV_TIMER_KEEPALIVE`   | Keepalive interval (20 s)                      |

### Modem events

| Event              | Triggered by                    |
|--------------------|---------------------------------|
| `EV_TX_STARTED`    | `arq_modem_ptt_on()` (PTT ON)   |
| `EV_TX_COMPLETE`   | `arq_modem_ptt_off()` (PTT OFF) |

---

## Session Roles: ISS and IRS

- **ISS** (Information Sending Station): the side currently transmitting data frames.
- **IRS** (Information Receiving Station): the side currently receiving and ACKing.

At connection establishment:
- **Caller** (side that issued `CONNECT`) → starts as **ISS** (sends data first).
- **Callee** (side in `LISTEN ON`) → starts as **IRS** (receives first).

This matches the VARA protocol convention and the UUCP use case where the
connecting side sends the session-initiation packets first.

---

## Turn Mechanism

Either side may request a role reversal.

**Piggyback turn** (fast path — single frame overhead):
1. IRS has TX data pending.
2. IRS sets `ARQ_FLAG_HAS_DATA` in the ACK frame.
3. ISS receives ACK with `HAS_DATA` → immediately transitions to IRS,
   IRS transitions to ISS and starts sending.

**Explicit TURN_REQ / TURN_ACK** (when piggyback is not available):
1. IRS sends `TURN_REQ` frame.
2. ISS stops sending, sends `TURN_ACK`.
3. Both sides swap roles.

The `ARQ_PEER_PAYLOAD_HOLD_S` constant (15 s) prevents the ISS from immediately
downgrading the modem mode back to DATAC13 while the peer is expected to have
more data.

**Queued local data fast turn**
- When the local side is IRS and new application data arrives, Mercury no longer
  waits the full 15 s peer-hold window before asking for the turn.
- If the peer has already been silent for `ARQ_TURN_WAIT_AFTER_ACK_MS` (3.5 s),
  IRS sends `TURN_REQ` immediately.
- If the data arrives earlier than that, Mercury shortens the peer-hold timer to
  the 3.5 s mark.  This preserves a safe receive window for a possible next ISS
  DATA frame, but avoids long idle gaps during interactive replies.

---

## Adaptive Rate Rules

Mode selection is deliberately conservative for HF.  A loopback cable can carry
DATAC1 immediately, but a real shortwave path can fade, drift, or lose a single
ACK even when the average SNR looks good.  The ARQ mode ladder therefore treats
DATAC3 as the fast interactive default and DATAC1 as an earned high-speed mode.

**Bandwidth caps**
- `BW500` is always clamped to DATAC4.
- `BW2300` and `BW2750` may use DATAC4, DATAC3, and DATAC1.
- Control frames continue to use DATAC13.

**Startup**
- New ARQ sessions start with DATAC4 payload mode.
- During `ARQ_STARTUP_MAX_S` (8 s), payload mode upgrades are suppressed while
  the link establishes stable control framing.
- At least one peer SNR report is required before any payload upgrade decision.

**DATAC3 upgrade**
- Allowed on `BW2300`/`BW2750` when peer SNR is at least
  `ARQ_SNR_MIN_DATAC3_DB + ARQ_SNR_HYST_DB`.
- Requires backlog >= `ARQ_BACKLOG_MIN_DATAC3` (1 byte), so short chat messages
  are not trapped on DATAC4.
- Does not require prior clean ACK history; this keeps interactive text usable.

**DATAC1 upgrade**
- Allowed only on `BW2300`/`BW2750`.
- Requires peer SNR at least `ARQ_SNR_MIN_DATAC1_DB + ARQ_SNR_HYST_DB`.
- Requires backlog >= `ARQ_BACKLOG_MIN_DATAC1` (126 bytes).
- Requires `speed_level >= ARQ_DATAC1_MIN_STABILITY_LEVEL`.
- `speed_level` rises only after `ARQ_LADDER_UP_SUCCESSES` (4) consecutive
  clean ACKs, so DATAC1 is used only after the link has proven itself.
- High-confidence fast path: if peer SNR is at least `ARQ_DATAC1_FAST_SNR_DB`
  (10 dB), there is a large backlog, and at least
  `ARQ_DATAC1_FAST_CLEAN_ACKS` (1) clean ACK has arrived without retry history,
  Mercury may use DATAC1 before the normal ladder step is earned.  Any retry
  disables this shortcut until clean delivery resumes.

**Downgrade and retry behavior**
- Any DATA retry resets `speed_level` to 0, forcing DATAC1 to be re-earned.
- `ARQ_RETRY_DOWNGRADE_THRESHOLD` (2) consecutive retries force a payload mode
  downgrade and start `ARQ_MODE_HOLD_AFTER_DOWNGRADE_S` (15 s) before re-upgrade.
- If peer SNR falls below the current mode threshold, the selector drops to a
  lower payload mode even without retry history.

**Mode switching**
- Current Mercury peers scan DATAC4/DATAC3/DATAC1 simultaneously while connected
  on wide links, so the ISS can switch payload mode directly before sending DATA.
- `MODE_REQ`/`MODE_ACK` is still understood for compatibility, but the local
  fast path avoids spending an extra control-frame round trip just to change
  speed.

---

## Keepalive

When the link is idle (no data in either direction):
- Every `ARQ_KEEPALIVE_INTERVAL_S` (20 s), the current ISS sends a `KEEPALIVE` frame.
- The IRS side waits `ARQ_IRS_INACTIVITY_S` (1 minute) without received frames
  before probing with a `KEEPALIVE`, so stale links are detected before the GUI
  application idle disconnect timer fires.
- The peer responds with `KEEPALIVE_ACK`.
- If `ARQ_KEEPALIVE_MISS_LIMIT` (5) consecutive keepalives receive no reply,
  the session is torn down with a local DISCONNECT.

---

## Timing Instrumentation

`arq_timing.c` records timestamps for every significant event in `arq_timing_ctx_t`
and emits `[TMG]` log lines.  These allow precise OTA round-trip analysis.

### Recorded events

| Function                           | Log tag     | What it records                          |
|------------------------------------|-------------|------------------------------------------|
| `arq_timing_record_tx_queue`       | `tx_queue`  | Frame submitted to action queue          |
| `arq_timing_record_tx_start`       | `tx_start`  | PTT ON (frame on air)                    |
| `arq_timing_record_tx_end`         | `tx_end`    | PTT OFF + on-air duration                |
| `arq_timing_record_rf_tx_start`    | `rf_tx_start`| Local ARQ PTT ON                         |
| `arq_timing_record_rf_tx_end`      | `rf_tx_end` | Local ARQ PTT OFF + on-air duration      |
| `arq_timing_record_ack_rx`         | `ack_rx`    | ACK received + OTA RTT + peer SNR        |
| `arq_timing_record_data_rx`        | `data_rx`   | Data frame decoded (IRS side) + SNR      |
| `arq_timing_record_ack_tx`         | `ack_tx`    | ACK TX started (IRS side) + local delay  |
| `arq_timing_record_retry`          | `retry`     | Retry event + attempt number + reason    |
| `arq_timing_record_turn`           | `turn`      | Role change + reason                     |
| `arq_timing_record_connect`        | `connect`   | Session established                      |
| `arq_timing_record_disconnect`     | `disconnect`| Session ended + session totals           |

### Duty-cycle telemetry

The `disconnect` timing line includes aggregate channel-use metrics for the
whole ARQ session:

| Field             | Meaning                                                       |
|-------------------|---------------------------------------------------------------|
| `rf_air`          | Total local ARQ PTT-on airtime across DATA and control frames |
| `tx_air`          | Total local DATA-frame PTT-on airtime                         |
| `session`         | Time from ARQ connect to disconnect                           |
| `rf_duty`         | Local transmit duty cycle: `rf_air / session`                 |
| `data_duty`       | DATA-only transmit duty cycle: `tx_air / session`             |
| `payload_rate`    | Confirmed application bytes divided by local DATA airtime     |
| `avg_ack_wait`    | Average time from local PTT-off to the peer ACK being decoded |
| `avg_ack_delay`   | Average peer-reported data-rx to ACK-tx delay                 |

Use these fields before changing ARQ timing constants.  A high `rf_duty` during
idle means keepalive policy is wasting channel time; a low `payload_rate` with a
large `avg_ack_wait` during data transfer means the stop-and-wait DATA/ACK cycle
is the main bottleneck and should be addressed with a block-ACK or multi-frame
burst design rather than by blindly shrinking channel guards.

### OTA RTT computation

```
OTA_RTT = (ack_rx_ms - tx_start_ms) - ack_delay × 10 ms
```

`ack_delay` is the IRS-reported time between receiving the data frame and
starting the ACK transmission (byte 7 of the control frame header).

---

## Logging

Mercury uses an asynchronous ring-buffer logger (`common/hermes_log.c`).

### Log levels

| Level   | Tag   | When emitted                                       |
|---------|-------|----------------------------------------------------|
| DEBUG   | `DBG` | Internal FSM traces (`-v` verbose mode only)       |
| TIMING  | `TMG` | Protocol timing events (always when file/JSONL used)|
| INFO    | `INF` | General status (default level)                     |
| WARN    | `WRN` | Retries, degraded conditions                       |
| ERROR   | `ERR` | Fatal / unexpected errors                          |

### CLI flags

| Flag        | Effect                                                         |
|-------------|----------------------------------------------------------------|
| `-v`        | Set console log level to DEBUG (shows all `[DBG]` and `[TMG]` lines) |
| `-L <path>` | Write all log levels (DEBUG+) to file at `<path>`             |
| `-J`        | Combine with `-L`: write file in **JSONL** format for machine parsing |

Example for OTA analysis:

```sh
./mercury -v -L /tmp/mercury-session.log
```

JSONL example (one JSON object per line in the log file):

```json
{"ts":"14:42:36.900","uptime_ms":12224,"level":"TMG","component":"arq","msg":"tx_start seq=0 mode=18 backlog=54"}
{"ts":"14:42:39.838","uptime_ms":15162,"level":"TMG","component":"arq","msg":"tx_end seq=0 dur_ms=2938"}
{"ts":"14:42:42.110","uptime_ms":17434,"level":"TMG","component":"arq","msg":"ack_rx seq=0 rtt_ms=2272 ack_delay_ms=410 peer_snr=2.2dB"}
```

Parse with `jq`:

```sh
jq 'select(.level == "TMG")' /tmp/mercury-session.log
```

---

## Tuning Guide

Use the `[TMG]` timing lines from a real OTA session to verify and adjust
the mode timing table in `arq_protocol.c`.

### Key measurements to check

1. **`tx_end` duration** (`dur_ms`) — compare against `frame_duration_s` in the table.
   If actual TX is longer, increase `frame_duration_s` and `ack_timeout_s` accordingly.

2. **`ack_rx` RTT** — should be less than `ack_timeout_s × 1000` ms.
   If RTT regularly exceeds ack_timeout, retries will be spurious.
   Rule of thumb: `ack_timeout_s ≥ frame_duration_s + 0.4 + ack_return_duration`.

3. **Retry rate** — a `[WRN] Data retry` log with `left=N` where N is near max_retries
   (`ARQ_DATA_RETRY_SLOTS = 10`) indicates marginal timing.  Increase `ack_timeout_s`.

4. **Turn latency** — time between `tx_end` on ISS and first `tx_start` on new ISS.
   Should be < 2 × `ARQ_CHANNEL_GUARD_MS` (400 ms).

### Common issues

| Symptom                                 | Likely cause                              | Fix                                    |
|-----------------------------------------|-------------------------------------------|----------------------------------------|
| Spurious retries despite good SNR       | `ack_timeout_s` too short                 | Increase by 1–2 s in mode table        |
| Link stuck at DATAC4, no mode upgrade   | SNR not high enough, or backlog too small | Check `ARQ_BACKLOG_MIN_DATAC3`         |
| UUCP handshake fails (timeout)          | First few data frames lost in startup     | Increase `ARQ_STARTUP_MAX_S` or reduce startup guard behaviour |
| Long gaps between turns                 | Early turn request not firing, or peer still transmitting | Check `ARQ_TURN_WAIT_AFTER_ACK_MS` and turn logs |
| Keepalive disconnects on idle link      | Propagation gap > keepalive window        | Increase `ARQ_KEEPALIVE_MISS_LIMIT`    |

### Timing constants quick reference

All in `arq_protocol.h`:

```c
#define ARQ_CHANNEL_GUARD_MS          700   /* IRS response guard after decode  */
#define ARQ_ISS_POST_ACK_GUARD_MS     900   /* ISS guard before DATA after ACK  */
#define ARQ_TURN_WAIT_AFTER_ACK_MS   3500   /* queued IRS data fast-turn wait   */
#define ARQ_ACK_GUARD_S               1     /* slack added to retry interval    */
#define ARQ_CALL_RETRY_SLOTS_DEFAULT  4     /* CALL retries                     */
#define ARQ_DATA_RETRY_SLOTS_DEFAULT 10     /* DATA retries before disconnect   */
#define ARQ_DISCONNECT_RETRY_SLOTS_DEFAULT 2
#define ARQ_KEEPALIVE_INTERVAL_S      20
#define ARQ_KEEPALIVE_MISS_LIMIT      5
#define ARQ_STARTUP_MAX_S             8     /* DATAC13-only startup window      */
#define ARQ_PEER_PAYLOAD_HOLD_S       15    /* hold payload mode after activity */
#define ARQ_IRS_INACTIVITY_CYCLES     4     /* 15s * 4 = 1 minute               */
#define ARQ_SNR_HYST_DB               1.0f
#define ARQ_DATAC1_FAST_SNR_DB       10.0f
#define ARQ_DATAC1_FAST_CLEAN_ACKS    1
```

---

## Source Files

| File                         | Responsibility                                      |
|------------------------------|-----------------------------------------------------|
| `datalink_arq/arq.c`         | Public API, event-loop thread, TCP bridge threads   |
| `datalink_arq/arq.h`         | Public API (VARA-compatible, frozen)                |
| `datalink_arq/arq_fsm.c`     | FSM state handlers and transition logic             |
| `datalink_arq/arq_fsm.h`     | State/event enums, `arq_session_t`, FSM API         |
| `datalink_arq/arq_protocol.c`| Frame builders, parsers, mode timing table          |
| `datalink_arq/arq_protocol.h`| Wire format constants, codec API                    |
| `datalink_arq/arq_timing.c`  | Timing recorder; emits `[TMG]` log lines            |
| `datalink_arq/arq_timing.h`  | `arq_timing_ctx_t`, recorder API                    |
| `datalink_arq/arq_modem.c`   | Action queue (FSM→modem TX), PTT event injection    |
| `datalink_arq/arq_modem.h`   | Action queue and PTT API                            |
| `datalink_arq/arq_channels.c`| Channel bus between TCP layer and event loop        |
| `common/hermes_log.c`        | Async ring-buffer logger with TIMING level and JSONL|
| `common/hermes_log.h`        | Logger API and `HLOGD/T/I/W/E` macros               |
| `modem/framer.c`             | 3-bit packet_type + 5-bit extension-field encoding  |
| `common/crc6.c`              | CRC helper implementations (legacy CRC5 still lives here) |

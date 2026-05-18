#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
MERCURY_BIN=${MERCURY_BIN:-"$ROOT_DIR/mercury"}
DEVICE=${MERCURY_LOOPBACK_DEVICE:-"BlackHole 2ch"}
MODE=${MERCURY_LOOPBACK_MODE:-2}
DURATION=${MERCURY_LOOPBACK_DURATION:-18}
RX_PORT=${MERCURY_LOOPBACK_RX_PORT:-18300}
TX_PORT=${MERCURY_LOOPBACK_TX_PORT:-18400}
RX_BCAST_PORT=${MERCURY_LOOPBACK_RX_BCAST_PORT:-18100}
TX_BCAST_PORT=${MERCURY_LOOPBACK_TX_BCAST_PORT:-18101}
TMP_BASE=${TMPDIR:-/tmp}
RUN_DIR="$TMP_BASE/mercury-loopback.$$"
RX_LOG="$RUN_DIR/rx.log"
TX_LOG="$RUN_DIR/tx.log"
rx_pid=
tx_pid=

cleanup() {
    if [ -n "${tx_pid:-}" ]; then
        kill "$tx_pid" 2>/dev/null || true
    fi
    if [ -n "${rx_pid:-}" ]; then
        kill "$rx_pid" 2>/dev/null || true
    fi
}

trap cleanup EXIT INT TERM

mkdir -p "$RUN_DIR"

printf 'Loopback device: %s\n' "$DEVICE"
printf 'Mode index: %s\n' "$MODE"
printf 'Logs: %s\n' "$RUN_DIR"

"$MERCURY_BIN" -x coreaudio -i "$DEVICE" -o "$DEVICE" -m "$MODE" -r \
    -p "$RX_PORT" -b "$RX_BCAST_PORT" >"$RX_LOG" 2>&1 &
rx_pid=$!

sleep 2

"$MERCURY_BIN" -x coreaudio -i "$DEVICE" -o "$DEVICE" -m "$MODE" -t \
    -p "$TX_PORT" -b "$TX_BCAST_PORT" >"$TX_LOG" 2>&1 &
tx_pid=$!

sleep "$DURATION"
cleanup
trap - EXIT INT TERM
sleep 1

printf '\n--- RX summary ---\n'
grep -E 'I/O capture|I/O playback|Resampler|DECODED FRAME|Unsupported|error' "$RX_LOG" || true

printf '\n--- TX summary ---\n'
grep -E 'I/O capture|I/O playback|Resampler|Unsupported|error' "$TX_LOG" || true

if grep -q 'DECODED FRAME' "$RX_LOG"; then
    printf '\nPASS: loopback audio path decoded modem frames.\n'
    exit 0
fi

printf '\nFAIL: RX did not decode frames. Full logs are in %s\n' "$RUN_DIR"
exit 1

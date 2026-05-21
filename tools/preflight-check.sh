#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT_DIR"

if [ "${MERCURY_SKIP_UNIT_TESTS:-0}" != "1" ]; then
    printf '%s\n' '==> Running Mercury unit tests'
    make test
else
    printf '%s\n' '==> Skipping unit tests (MERCURY_SKIP_UNIT_TESTS=1)'
fi

if [ "${MERCURY_SKIP_LOOPBACK_SMOKE:-0}" != "1" ]; then
    printf '%s\n' '==> Running Mercury TNC A/B loopback smoke test'
    make tnc-loopback-smoke
else
    printf '%s\n' '==> Skipping TNC loopback smoke test (MERCURY_SKIP_LOOPBACK_SMOKE=1)'
fi

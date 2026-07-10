#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
LED_TEST_BIN="$REPO_ROOT/build/release/bin/pico-led-test"
PICODECODER_DIR="$REPO_ROOT/resources/picodecoder/linux/x86_64"

if [[ ! -x "$LED_TEST_BIN" ]]; then
    echo "Missing binary: $LED_TEST_BIN" >&2
    echo "Build it with: cmake -B \"$REPO_ROOT/build\" -DCMAKE_BUILD_TYPE=Release -DUSE_DYNAMIC=ON && cmake --build \"$REPO_ROOT/build\" --target pico-led-test -j" >&2
    exit 1
fi

export LD_LIBRARY_PATH="$PICODECODER_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

exec sudo env LD_LIBRARY_PATH="$LD_LIBRARY_PATH" "$LED_TEST_BIN" "$@"

#!/bin/sh
# Compiles the idle boiler-fill control out of the sketch and runs it on the
# host.
#
# Checking this on the machine means a cold start per iteration, so the logic
# that decides whether the GiCar may run the pump — the warm-up cut-off, the
# debounce, the latch timeout and the filter settle window — is exercised here
# instead. extract.py lifts the functions and their tunables straight out of
# marax_esp32s3.ino, so the tests cannot drift from what gets flashed.
#
# Run from the repository root:  sh test/idle_pump/run.sh
set -e
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
python3 test/idle_pump/extract.py "$OUT/idle_pump.h"
c++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -I"$OUT" \
    -o "$OUT/test_idle_pump" test/idle_pump/test_idle_pump.cpp
"$OUT/test_idle_pump"

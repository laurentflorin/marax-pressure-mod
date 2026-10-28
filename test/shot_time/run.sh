#!/bin/sh
# Compiles the checks that decide whether a brew-by-weight shot may time its
# recipe out of the sketch and runs them on the host.
#
# A time that slips through stops every later shot without the scale at the
# wrong moment, and the failures it has to catch — an untared scale, a bump, a
# stalled stream — are tedious to stage at the machine. extract.py lifts the
# functions and their tunables straight out of marax_esp32s3.ino, so the tests
# cannot drift from what gets flashed.
#
# Run from the repository root:  sh test/shot_time/run.sh
set -e
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
python3 test/shot_time/extract.py "$OUT/shot_time.h"
c++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -I"$OUT" \
    -o "$OUT/test_shot_time" test/shot_time/test_shot_time.cpp
"$OUT/test_shot_time"

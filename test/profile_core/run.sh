#!/bin/sh
# Compiles the profile maths out of the sketch and runs it on the host.
#
# The pressure-profile core — CSV parsing, ramp/jump interpolation, the manual
# step conversion — is plain arithmetic with no hardware in it, so it can be
# checked without flashing the board. extract.py lifts those functions straight
# out of marax_esp32s3.ino by brace matching, so the tests always run against
# the real source rather than a copy that can drift.
#
# Run from the repository root:  sh test/profile_core/run.sh
set -e
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
python3 test/profile_core/extract.py "$OUT/profile_core.h"
c++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -I"$OUT" \
    -o "$OUT/test_profile" test/profile_core/test_profile.cpp
"$OUT/test_profile"

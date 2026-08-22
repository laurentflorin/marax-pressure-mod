#!/bin/sh
# Checks the Lovelace card against the firmware: the curve it draws must be the
# curve the pump follows. Needs node and a C++ compiler.
#
# Run from the repository root:  sh test/card/run.sh
set -e
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
node --check homeassistant/marax-profile-card.js
python3 test/profile_core/extract.py "$OUT/profile_core.h"
c++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -I"$OUT" \
    -o "$OUT/dump_curve" test/profile_core/dump_curve.cpp
node test/card/compare.js "$OUT/dump_curve"

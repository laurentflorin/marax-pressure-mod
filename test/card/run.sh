#!/bin/sh
# Checks the recipe card against the firmware: it must read a recipe exactly
# as the machine does, and draw the curve the pump follows. Needs node and a
# C++ compiler.
#
# Run from the repository root:  sh test/card/run.sh
set -e
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
node --check homeassistant/marax-recipe-card.js
python3 test/profile_core/extract.py "$OUT/profile_core.h"
c++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -I"$OUT" \
    -o "$OUT/dump_recipe" test/profile_core/dump_recipe.cpp
node test/card/compare_recipe.js "$OUT/dump_recipe"

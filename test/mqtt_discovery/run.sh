#!/bin/sh
# Checks that every Home Assistant discovery payload fits PubSubClient's send
# buffer. An oversized payload is dropped silently, so the only symptom on the
# machine is an entity that never appears.
#
# Run from the repository root:  sh test/mqtt_discovery/run.sh
set -e
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
python3 test/mqtt_discovery/extract.py "$OUT/discovery.h"
c++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -I"$OUT" \
    -o "$OUT/test_discovery" test/mqtt_discovery/test_discovery.cpp
"$OUT/test_discovery"

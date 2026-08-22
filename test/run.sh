#!/bin/sh
# Runs every host-side test. None of them need the board: the pressure-profile
# core, the MQTT payload sizes and the Lovelace card's curve maths are all
# plain computation, and the test code is lifted straight out of the sketch so
# it cannot drift from what actually gets flashed.
#
# Needs python3, a C++ compiler, and node (for the card test). The compile
# suite also needs the ESP32 toolchain, and skips itself if it is not there.
#
#   sh test/run.sh
set -e
cd "$(dirname "$0")/.."
for suite in profile_core mqtt_discovery card compile; do
  echo "── $suite ─────────────────────────────────────────────"
  sh "test/$suite/run.sh"
  echo
done
echo "everything passed"

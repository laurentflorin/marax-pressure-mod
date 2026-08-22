#!/bin/sh
# Syntax-checks the sketch with the real ESP32-S3 toolchain.
#
# The other suites test the profile maths on the host, which cannot catch
# anything about the sketch as a whole — a macro used above where it is
# defined, a missing include, a typo in a branch nothing exercises. This one
# runs the actual xtensa compiler over the whole file with the include and
# define set Arduino IDE uses for "ESP32S3 Dev Module", so those show up here
# instead of at flashing time.
#
# It reuses the toolchain the Arduino IDE already downloaded; if that is not
# installed it skips rather than failing. Override the locations with
# ARDUINO15 and ARDUINO_LIBS.
#
# Run from the repository root:  sh test/compile/run.sh
set -e
cd "$(dirname "$0")/../.."

ARDUINO15=${ARDUINO15:-$HOME/.arduino15}
USERLIB=${ARDUINO_LIBS:-$HOME/Arduino/libraries}
SKETCH=src/marax_esp32s3/marax_esp32s3.ino

skip() {
  echo "SKIP: $1"
  echo "      Install the ESP32 board package in Arduino IDE, or set ARDUINO15."
  exit 0
}

[ -d "$ARDUINO15/packages/esp32" ] || skip "no ESP32 board package under $ARDUINO15"

CORE=$(ls -d "$ARDUINO15"/packages/esp32/hardware/esp32/*/ 2>/dev/null | sort -V | tail -1)
[ -n "$CORE" ] || skip "no ESP32 core installed"
VERSION=$(basename "$CORE")
LIBS="$ARDUINO15/packages/esp32/tools/esp32s3-libs/$VERSION"
[ -d "$LIBS" ] || skip "no esp32s3-libs for core $VERSION"

CXX=$(ls "$ARDUINO15"/packages/esp32/tools/esp-x32/*/bin/xtensa-esp32s3-elf-g++ 2>/dev/null | sort -V | tail -1)
[ -x "$CXX" ] || skip "no xtensa-esp32s3-elf-g++ toolchain"

for lib in PubSubClient NimBLE-Arduino Easy_Nextion_Library; do
  [ -d "$USERLIB/$lib/src" ] || skip "library $lib not found under $USERLIB"
done

echo "core $VERSION"

# The board is 16 MB flash with OPI PSRAM, which selects the qio_opi sdkconfig.
INC="-iprefix $LIBS/include/ @$LIBS/flags/includes"
INC="$INC -I$LIBS/qio_opi/include -I$LIBS/include"
INC="$INC -I${CORE}cores/esp32 -I${CORE}variants/esp32s3"
for dir in "$CORE"libraries/*/src; do INC="$INC -I$dir"; done
for lib in PubSubClient NimBLE-Arduino Easy_Nextion_Library; do INC="$INC -I$USERLIB/$lib/src"; done

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cp "$(dirname "$SKETCH")"/*.h "$WORK/"
# The Arduino builder injects prototypes for every function in a .ino; without
# them everything used before its definition looks undeclared.
python3 test/compile/prototypes.py "$SKETCH" "$WORK/sketch.cpp"

"$CXX" -fsyntax-only -std=gnu++2b -c \
  @"$LIBS/flags/defines" @"$LIBS/flags/cpp_flags" \
  -DF_CPU=240000000L -DARDUINO=10607 -DARDUINO_ESP32S3_DEV \
  -DARDUINO_ARCH_ESP32 -DESP32 -DCORE_DEBUG_LEVEL=0 \
  -DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1 -DBOARD_HAS_PSRAM \
  -DARDUINO_BOARD='"ESP32S3_DEV"' -DARDUINO_VARIANT='"esp32s3"' \
  $INC -I"$WORK" "$WORK/sketch.cpp"

echo "sketch compiles"

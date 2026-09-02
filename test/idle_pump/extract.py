# -*- coding: utf-8 -*-
import io, re, sys

src = io.open("src/marax_esp32s3/marax_esp32s3.ino", encoding="utf-8").read()

def grab(signature_re):
    m = re.search(signature_re + r"\([^)]*\)\s*\n\{", src, re.M)
    if not m:
        sys.exit("not found: " + signature_re)
    i = src.index("{", m.start())
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{": depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[m.start():j+1] + "\n"
    sys.exit("unbalanced: " + signature_re)

def span(start_marker, end_marker):
    """Everything from start_marker to the end of the statement end_marker
    names. Neither marker includes a value, so retuning a constant does not
    silently drop it out of the extraction."""
    a = src.find(start_marker)
    if a < 0:
        sys.exit("not found: " + start_marker)
    b = src.find(end_marker, a)
    if b < 0:
        sys.exit("not found after start: " + end_marker)
    return src[a:src.index(";", b) + 1] + "\n"

# The thresholds and the latch state are the thing under test, so they are
# lifted from the sketch rather than restated here.
tunables = span("const float BOILER_FULL_BAR_COLD", "PRESSURE_SETTLE_MS")
state = span("bool boilerFullLatch", "pressureFilterPrimedAtMs")

prelude = '''#include <cstdint>
#include <cstdio>
#include <cstring>

// ── Hardware stubs ────────────────────────────────────────────────────────
// The test drives the clock and the pressure reading directly; everything the
// control logic touches beyond that is inert.
static unsigned long testNowMs = 0;
static float testPressureBar = 0.0f;
static unsigned long millis() { return testNowMs; }
static float getPressure() { return testPressureBar; }

#define PRESSURE_SENSOR_PIN 3
static float sensorVal = 0, filteredVal = 0, voltage = 0;
static int analogRead(int) { return 496; }  // sensor zero, 0.4 V

struct SerialStub {
  void print(const char *) {}
  void print(int) {}
  void print(unsigned long) {}
  void print(float, int) {}
  void println(const char *) {}
  void println() {}
};
static SerialStub Serial;

// ── Sketch state the logic reads ──────────────────────────────────────────
static bool brewActive = false;
static bool POWER_ON = false;
static bool cleaningModeActive = false;
static bool cleaningRunActive = false;
static int steamTemp = 0;
static uint8_t pumpBrightness = 255;
static int setPumpBrightnessCalls = 0;
static void setPumpBrightness(uint8_t b) {
  if (b != pumpBrightness) setPumpBrightnessCalls++;
  pumpBrightness = b;
}

float boilerFullPressureBar = 1.0f;
'''

out = sys.argv[1]
io.open(out, "w", encoding="utf-8").write(
    prelude + "\n" + tunables + "\n" + state + "\n"
    + grab(r"^void resumeIdleBoilerFill")
    + grab(r"^void primePressureFilter")
    + grab(r"^void updateIdlePumpControl"))
print("extracted the idle pump control out of the sketch")

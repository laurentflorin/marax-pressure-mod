// Tests the idle boiler-fill control lifted straight out of the sketch.
//
// The bug this suite pins down: during warm-up the cut-off was 0.5 bar, which
// residual line pressure and thermal expansion clear on their own, and the
// resulting latch held until the next brew — so the machine would sit there
// with a full tank and never fill the boiler.
//
// Every expectation below is written in absolute pressures and absolute times,
// never in terms of the tunables themselves, so retuning a constant out of its
// useful range fails the suite instead of quietly moving the goalposts with it.
#include "idle_pump.h"

// The envelope the tests assume the tunables live in. The thresholds are
// `const float`, which C++ will not fold into a constant expression, so those
// two are checked at runtime alongside everything else.
static_assert(BOILER_FULL_DEBOUNCE_MS > 300 && BOILER_FULL_DEBOUNCE_MS < 1500,
              "the debounce must outlast a noisy sample without stalling a real cut");
static_assert(BOILER_FULL_LATCH_TIMEOUT_MS > 10000 && BOILER_FULL_LATCH_TIMEOUT_MS < 120000,
              "the latch must outlast a refill cycle but heal within a warm-up");
static_assert(PRESSURE_SETTLE_MS > 500 && PRESSURE_SETTLE_MS + BOILER_FULL_DEBOUNCE_MS < 3000,
              "the settle window must cover the seed sample without blinding the cut-off");

static int failures = 0;

static void check(bool ok, const char *what)
{
  printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) failures++;
}

// Advances the clock in `stepMs` slices, running the control each time, the
// way loop() does.
static void run(unsigned long totalMs, unsigned long stepMs = 50)
{
  for (unsigned long t = 0; t < totalMs; t += stepMs)
  {
    testNowMs += stepMs;
    updateIdlePumpControl();
  }
}

// A machine that has just been switched on: powered, cold, filter settled.
static void idleMachine(int steam = 25)
{
  testNowMs = 100000;
  testPressureBar = 0.0f;
  brewActive = false;
  POWER_ON = true;
  cleaningModeActive = false;
  cleaningRunActive = false;
  steamTemp = steam;
  pumpBrightness = 255;
  setPumpBrightnessCalls = 0;
  resumeIdleBoilerFill();
  pressureFilterPrimedAtMs = 0;  // settle window long past
}

int main()
{
  printf("\nthe cut-off thresholds sit where the tests assume\n");
  check(BOILER_FULL_BAR_COLD >= 1.0f,
        "the cold cut-off clears ordinary warm-up line pressure");
  check(BOILER_FULL_BAR_HOT > BOILER_FULL_BAR_COLD,
        "a hot boiler sits above the cold cut-off by design");

  printf("\ncold warm-up must not cut the fill\n");

  idleMachine();
  testPressureBar = 0.7f;  // residual line pressure — over the old 0.5 bar cut-off
  run(5000);
  check(pumpBrightness == 255, "0.7 bar of residual pressure leaves the pump running");
  check(!boilerFullLatch, "...and does not latch");

  idleMachine();
  testPressureBar = 0.9f;  // thermal expansion as the HX heats
  run(60000);
  check(pumpBrightness == 255, "a minute at 0.9 bar still leaves the pump running");

  idleMachine();
  testPressureBar = 3.0f;  // one noisy sample
  updateIdlePumpControl();
  testPressureBar = 0.2f;
  run(2000);
  check(pumpBrightness == 255, "a single spike above the cut-off is debounced away");
  check(!boilerFullLatch, "...and does not latch");

  printf("\nreal over-pressure still cuts the pump\n");

  idleMachine();
  testPressureBar = 2.0f;
  run(1500);
  check(boilerFullLatch, "2 bar held for 1.5s latches");
  check(pumpBrightness == 0, "...and cuts the pump");

  idleMachine();
  testPressureBar = 2.0f;
  run(300);
  check(pumpBrightness == 255, "...but 300ms of it is not enough to cut");

  idleMachine(120);  // steam hot
  testPressureBar = 3.0f;
  run(10000);
  check(boilerFullPressureBar == BOILER_FULL_BAR_HOT, "a hot machine uses the hot cut-off");
  check(pumpBrightness == 255, "...so 3 bar of boiler pressure does not cut the pump");

  printf("\na latch the pressure stops backing up is released\n");

  idleMachine();
  testPressureBar = 2.0f;
  run(1500);
  check(pumpBrightness == 0, "latched on real pressure");
  testPressureBar = 0.0f;
  run(10000, 500);
  check(pumpBrightness == 0, "...still held 10s later, so a refill cycle is not disturbed");
  run(110000, 500);
  check(!boilerFullLatch, "...but released within two minutes");
  check(pumpBrightness == 255, "...handing the pump back to the GiCar");

  idleMachine();
  testPressureBar = 2.0f;
  run(300000, 500);  // five minutes of genuine boiler pressure
  check(pumpBrightness == 0, "a latch pressure keeps justifying never times out");

  idleMachine();
  testPressureBar = 2.0f;
  run(1500);
  check(pumpBrightness == 0, "latched before a brew");
  resumeIdleBoilerFill();  // what the brew→idle edge does
  check(!boilerFullLatch && pumpBrightness == 255, "the end of a brew releases it at once");

  printf("\nthe filter's seed sample cannot latch anything\n");

  idleMachine();
  primePressureFilter();   // what the POWER_ON edge does
  testPressureBar = 4.0f;  // seeded from a bad sample
  run(500);
  check(pumpBrightness == 255, "no latch while the filter is still converging");
  check(!boilerFullLatch, "...and no latch state left behind");
  run(3000);
  check(pumpBrightness == 0, "...but a reading that persists past the window still cuts");

  printf("\nthe dimmer stays open when the cut-off must not apply\n");

  idleMachine();
  POWER_ON = false;
  testPressureBar = 8.0f;
  run(5000);
  check(pumpBrightness == 255, "a machine that is not reporting keeps the dimmer open");

  idleMachine();
  cleaningModeActive = true;
  testPressureBar = 8.0f;
  run(5000);
  check(pumpBrightness == 255, "backflush pressure does not fight the cleaning cycle");

  idleMachine();
  testPressureBar = 8.0f;
  brewActive = true;
  run(5000);
  check(pumpBrightness == 255, "a brew leaves the dimmer to pressureProfile()");

  printf("\n%s\n\n", failures ? "FAILURES" : "all checks passed");
  return failures ? 1 : 0;
}

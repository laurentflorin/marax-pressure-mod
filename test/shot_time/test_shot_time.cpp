// Tests the checks, lifted straight out of the sketch, that decide whether a
// shot the weight cut-off stopped may time its recipe for shots without the
// scale.
//
// Each case is one shot as the scale would report it: the reading as the lever
// goes up, then a reading every so often until the cut-off. Expectations are
// written in grams, seconds and milliseconds, never in terms of the tunables,
// so retuning a constant out of its useful range fails the suite instead of
// quietly moving the goalposts with it.
#include "shot_time.h"

static int failures = 0;

static void check(bool ok, const char *what)
{
  printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) failures++;
}

static void checkKept(float seconds, float yield, const char *what)
{
  const char *rejection = shotTimeRejection(seconds, yield);
  check(rejection == NULL, what);
  if (rejection != NULL) printf("        rejected: %s\n", rejection);
}

static void checkRejected(float seconds, float yield, const char *what)
{
  const char *rejection = shotTimeRejection(seconds, yield);
  check(rejection != NULL, what);
  if (rejection != NULL) printf("        %s\n", rejection);
}

static unsigned long nowMs = 0;
static float weight = 0.0f;

// The lever goes up with the scale showing `grams`, its last reading `agoMs`
// old.
static void leverUp(float grams, bool scale = true, unsigned long agoMs = 50)
{
  nowMs = 100000;
  scaleConnected = scale;
  lastWeightTime = scale ? nowMs - agoMs : 0;
  currentWeight = grams;
  weight = grams;
  startShotEvidence();
}

// `seconds` of coffee at `gps` grams a second, a reading every `everyMs`.
static void pour(float seconds, float gps, unsigned long everyMs = 100)
{
  for (unsigned long t = everyMs; t <= (unsigned long)(seconds * 1000.0f); t += everyMs)
  {
    nowMs += everyMs;
    weight += gps * everyMs / 1000.0f;
    noteShotWeight(nowMs, weight);
  }
}

// One reading of `grams`, `afterMs` after the one before.
static void reading(unsigned long afterMs, float grams)
{
  nowMs += afterMs;
  weight = grams;
  noteShotWeight(nowMs, weight);
}

int main()
{
  printf("\nshots whose time is kept\n");
  leverUp(0.1f);
  pour(28.0f, 1.3f);
  checkKept(28.0f, 36.0f, "36 g in 28 s, read ten times a second");

  leverUp(-0.6f);
  pour(27.0f, 1.35f);
  checkKept(27.0f, 36.0f, "a scale that drifted half a gram below zero");

  leverUp(1.5f);
  pour(26.0f, 1.3f);
  checkKept(26.0f, 36.0f, "drips from the last shot still in the cup");

  leverUp(0.0f);
  for (int i = 0; i < 280; i++)
  {
    // The Felicita's own wobble, a few tenths either way around the flow.
    reading(100, 1.3f * (i + 1) / 10.0f + ((i % 3) - 1) * 0.3f);
  }
  checkKept(28.0f, 36.0f, "jitter of a few tenths of a gram");

  leverUp(0.0f);
  const unsigned long gaps[] = {100, 100, 450, 3, 100, 300, 100, 900};
  float poured = 0.0f;
  for (int i = 0; i < 104; i++)
  {
    unsigned long gap = gaps[i % 8];
    poured += 1.3f * gap / 1000.0f;
    reading(gap, poured);
  }
  checkKept(26.7f, 36.0f, "readings bunched and delayed by the radio, up to 0.9 s apart");

  leverUp(0.0f);
  pour(15.0f, 3.6f);
  checkKept(15.0f, 54.0f, "a turbo shot: 54 g in 15 s");

  leverUp(0.0f);
  pour(5.0f, 0.0f);
  pour(25.0f, 1.5f);
  checkKept(30.0f, 36.0f, "a long preinfusion with nothing in the cup");

  printf("\nscales that were not ready\n");
  leverUp(0.0f, false);
  checkRejected(28.0f, 36.0f, "no scale when the lever went up");

  leverUp(152.0f);
  pour(1.0f, 1.3f);
  checkRejected(1.0f, 36.0f, "the cup put on after taring: the scale reads the cup");

  leverUp(-48.0f);
  pour(40.0f, 2.1f);
  checkRejected(40.0f, 36.0f, "tared with a heavier cup than the one under the spout");

  leverUp(3.5f);
  pour(24.0f, 1.3f);
  checkRejected(24.0f, 36.0f, "three and a half grams on the scale as the lever went up");

  printf("\nsomething other than coffee moving the scale\n");
  leverUp(0.0f);
  pour(2.0f, 0.0f);
  reading(100, 151.0f);
  checkRejected(2.1f, 36.0f, "the cup set down after the lever went up");

  leverUp(0.0f);
  pour(15.0f, 1.3f);
  reading(100, weight + 20.0f);
  checkRejected(15.1f, 36.0f, "a knock on the cup that tips the reading over the target");

  leverUp(0.0f);
  pour(15.0f, 1.3f);
  reading(100, weight + 20.0f);
  reading(100, weight - 20.0f);
  pour(10.0f, 1.3f);
  checkRejected(25.2f, 36.0f, "a knock that came and went, long before the cut-off");

  leverUp(0.0f);
  pour(12.0f, 1.3f);
  pour(1.0f, 20.0f);
  checkRejected(13.0f, 36.0f, "a hand leaning on the cup, 20 g/s on top of the flow");

  leverUp(0.0f);
  pour(10.0f, 1.3f);
  reading(100, 0.0f);
  pour(28.0f, 1.3f);
  checkRejected(38.1f, 36.0f, "tared mid-shot, so the shot ran 13 g long");

  leverUp(0.0f);
  pour(10.0f, 1.3f);
  reading(100, 13.0f - 140.0f);
  reading(1200, 13.0f + 2.0f);
  pour(16.0f, 1.3f);
  checkRejected(27.3f, 36.0f, "the cup lifted to look and put back");

  printf("\nthe scale going quiet\n");
  leverUp(0.0f);
  pour(10.0f, 1.3f);
  reading(3000, weight + 3.9f);
  pour(15.0f, 1.3f);
  checkRejected(28.0f, 36.0f, "three seconds without a reading");

  leverUp(0.0f, true, 3000);
  pour(28.0f, 1.3f);
  checkRejected(28.0f, 36.0f, "its last reading already three seconds old at the lever");

  leverUp(0.0f);
  pour(24.0f, 1.3f);
  reading(4000, 36.0f);
  checkRejected(28.0f, 36.0f, "the reading that tipped it over came after four quiet seconds");

  printf("\ntimes that cannot be a real shot\n");
  leverUp(0.0f);
  pour(6.0f, 6.0f);
  checkRejected(6.0f, 36.0f, "36 g in 6 s");

  leverUp(0.0f);
  pour(12.0f, 12.5f);
  checkRejected(12.0f, 150.0f, "150 g in 12 s: each reading plausible, the whole not");

  leverUp(0.0f);
  pour(9.0f, 1.0f);
  checkRejected(9.0f, 9.0f, "anything under ten seconds, even a 9 g ristretto");

  leverUp(0.0f);
  pour(130.0f, 0.28f);
  checkRejected(130.0f, 36.0f, "a choked puck that took over two minutes");

  leverUp(0.0f);
  pour(100.0f, 0.36f);
  checkKept(100.0f, 36.0f, "...but a very slow one inside the limit is still a shot");

  printf("\none shot to the next\n");
  leverUp(0.0f);
  pour(5.0f, 1.3f);
  reading(100, weight + 30.0f);
  pour(20.0f, 1.3f);
  checkRejected(25.2f, 36.0f, "a bump is not forgotten by the readings after it");
  leverUp(0.2f);
  pour(28.0f, 1.3f);
  checkKept(28.0f, 36.0f, "the next lever-up starts clean");

  printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
  return failures != 0;
}

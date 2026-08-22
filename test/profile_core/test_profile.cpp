#include "profile_core.h"
#include <vector>
#include <fstream>
#include <sstream>

static int failures = 0;
static void check(bool ok, const char *what) {
  std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) failures++;
}
static void checkNear(float got, float want, const char *what) {
  bool ok = std::fabs(got - want) < 0.01f;
  std::printf("  %s  %-52s got %.3f want %.3f\n", ok ? "PASS" : "FAIL", what, got, want);
  if (!ok) failures++;
}

// Mirrors loadProfile()'s dispatch without the SD layer.
static bool loadFromText(const std::string &text) {
  std::istringstream in(text);
  std::string raw;
  std::getline(in, raw);
  String header(raw);
  if (header.length() >= 3 && (uint8_t)header.charAt(0) == 0xEF &&
      (uint8_t)header.charAt(1) == 0xBB && (uint8_t)header.charAt(2) == 0xBF) {
    header.remove(0, 3);
  }
  header.trim();
  profilePointCount = 0;
  profileTargetWeightField = 0.0f;

  if (isLegacyProfileHeader(header)) {
    std::string rowRaw; std::getline(in, rowRaw);
    String row(rowRaw); row.trim();
    return parseLegacyProfileRow(row);
  }
  String line = header;
  while (line.length() > 0) {
    line.trim();
    if (line.length() > 0 && line.charAt(0) != '#') {
      char buf[PROFILE_CSV_BUFFER_LEN];
      line.toCharArray(buf, sizeof(buf));
      if (!parseProfileV2Line(buf)) return false;
    }
    std::string next;
    if (!std::getline(in, next)) break;
    line = String(next);
  }
  return profilePointCount >= PROFILE_MIN_POINTS;
}

int main() {
  // ── The user's stated shape: hold 3, jump to 8, hold, ramp down to 6, hold ──
  std::printf("\nramp/jump mix\n");
  check(loadFromText(
    "# marax profile v2\n"
    "name,Mixed\n"
    "target_weight,36.0\n"
    "point,0,3,ramp\n"
    "point,6,8,jump\n"
    "point,14,8,ramp\n"
    "point,22,6,ramp\n"
    "point,30,6,ramp\n"), "parses");
  check(profilePointCount == 5, "5 points");
  checkNear(profileTotalSeconds(), 30.0f, "total duration");
  checkNear(profileTargetPressureAt(0),    3.0f, "t=0 starts at 3 bar");
  checkNear(profileTargetPressureAt(5.9f), 3.0f, "t=5.9 still 3 bar (held, no ramp up)");
  checkNear(profileTargetPressureAt(6),    8.0f, "t=6 jumps to 8 bar");
  checkNear(profileTargetPressureAt(10),   8.0f, "t=10 holds 8 bar");
  checkNear(profileTargetPressureAt(14),   8.0f, "t=14 still 8 bar");
  checkNear(profileTargetPressureAt(18),   7.0f, "t=18 halfway down the ramp");
  checkNear(profileTargetPressureAt(22),   6.0f, "t=22 ramp reaches 6 bar");
  checkNear(profileTargetPressureAt(30),   6.0f, "t=30 end of profile");
  checkNear(profileTargetPressureAt(45),   6.0f, "past the end holds the last pressure");
  checkNear(profileTargetPressureAt(-5),   3.0f, "before the start holds the first");

  // ── Legacy files keep their staircase ──────────────────────────────────
  std::printf("\nlegacy default.csv (4,8 6,12 9,15 7,5)\n");
  check(loadFromText("name,t1p,t1t,t2p,t2t,t3p,t3t,t4p,t4t,target_weight\n"
                     "default,4,8,6,12,9,15,7,5,36.0\n"), "parses");
  checkNear(profileTotalSeconds(), 40.0f, "total duration matches 8+12+15+5");
  checkNear(profileTargetPressureAt(0),     4.0f, "step 1");
  checkNear(profileTargetPressureAt(7.9f),  4.0f, "step 1 held to its end");
  checkNear(profileTargetPressureAt(8),     6.0f, "step 2 begins");
  checkNear(profileTargetPressureAt(19.9f), 6.0f, "step 2 held flat, no ramp");
  checkNear(profileTargetPressureAt(20),    9.0f, "step 3 begins");
  checkNear(profileTargetPressureAt(35),    7.0f, "step 4 begins");
  checkNear(profileTargetPressureAt(60),    7.0f, "past the end holds step 4");
  checkNear(profileTargetWeightField, 36.0f, "target_weight preserved but unapplied");

  // ── Manual display steps produce the identical staircase ───────────────
  std::printf("\nmanual mode equivalence\n");
  t1p=4; t1t=8; t2p=6; t2t=12; t3p=9; t3t=15; t4p=7; t4t=5;
  applyManualStepsToProfile();
  checkNear(profileTotalSeconds(), 40.0f, "same duration as the legacy file");
  checkNear(profileTargetPressureAt(19.9f), 6.0f, "same value mid-step-2");
  checkNear(profileTargetPressureAt(35),    7.0f, "same value in step 4");

  // ── Rejections ─────────────────────────────────────────────────────────
  std::printf("\nmalformed input\n");
  check(!loadFromText("name,X\npoint,10,6\npoint,4,9\n"), "rejects out-of-order points");
  check(!loadFromText("name,X\npoint,0\n"),                "rejects a point missing its pressure");
  check(!loadFromText("name,X\npoint,0,6\n"),              "rejects a single-point profile");
  check(loadFromText("name,X\nwibble,1,2\npoint,0,3\npoint,10,9\n"), "ignores unknown keys");

  std::printf("\nclamping\n");
  check(loadFromText("name,X\npoint,0,99\npoint,9999,-4\n"), "parses out-of-range values");
  checkNear(profilePoints[0].bar, PROFILE_MAX_BAR, "pressure clamped to the ceiling");
  checkNear(profilePoints[1].bar, 0.0f, "negative pressure clamped to zero");
  checkNear(profilePoints[1].seconds, PROFILE_MAX_SECONDS, "time clamped to the ceiling");

  // ── Every shipped example profile still loads ──────────────────────────
  std::printf("\nshipped example profiles\n");
  const char *examples[] = {"blooming_espresso","classic_espresso","declining_pressure",
                            "default","pre_infusion_ramp","slayer_shot","turbo_espresso"};
  for (const char *name : examples) {
    std::ifstream f(std::string("sd_card_examples/profiles/") + name + ".csv");
    std::stringstream ss; ss << f.rdbuf();
    bool ok = loadFromText(ss.str());
    char label[128];
    std::snprintf(label, sizeof(label), "%s loads (%d points, %.0fs)",
                  name, profilePointCount, profileTotalSeconds());
    check(ok, label);
  }

  // ── The preview signature notices edits ────────────────────────────────
  std::printf("\nchange detection\n");
  loadFromText("name,X\npoint,0,3\npoint,10,9\n");
  uint32_t a = profileSegmentSignature();
  loadFromText("name,X\npoint,0,3\npoint,10,9,jump\n");
  uint32_t b = profileSegmentSignature();
  loadFromText("name,X\npoint,0,3\npoint,10,9.5\n");
  uint32_t c = profileSegmentSignature();
  check(a != b, "ramp vs jump changes the signature");
  check(a != c, "a 0.5 bar edit changes the signature");

  std::printf("\npressureToWave\n");
  check(pressureToWave(0.0f) == 0,    "0 bar maps to 0");
  check(pressureToWave(10.0f) == 180, "10 bar maps to full scale");
  check(pressureToWave(5.0f) == 90,   "5 bar maps to half scale");
  check(pressureToWave(12.0f) == 180, "over-scale is clamped");

  std::printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
  return failures != 0;
}

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

// Mirrors loadRecipe() without the SD layer: the text straight into the live
// curve, rejected when it does not describe one.
static bool loadFromText(const std::string &text) {
  profilePointCount = 0;
  resetRecipeScratch("test");
  return parseBodyLines(text.c_str()) && profilePointCount >= PROFILE_MIN_POINTS;
}

int main() {
  // ── Hold 3, jump to 8, hold, ramp down to 6, hold ──────────────────────
  std::printf("\nramp/jump mix\n");
  check(loadFromText(
    "# marax recipe v1\n"
    "name,Mixed\n"
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
  checkNear(profileTargetPressureAt(30),   6.0f, "t=30 end of the curve");
  checkNear(profileTargetPressureAt(45),   6.0f, "past the end holds the last pressure");
  checkNear(profileTargetPressureAt(-5),   3.0f, "before the start holds the first");

  // ── A staircase: every point a jump ────────────────────────────────────
  std::printf("\nstaircase (4,8 6,12 9,15 7,5)\n");
  check(loadFromText("point,0,4\npoint,8,6,jump\npoint,20,9,jump\npoint,35,7,jump\npoint,40,7,jump\n"),
        "parses");
  checkNear(profileTotalSeconds(), 40.0f, "total duration matches 8+12+15+5");
  checkNear(profileTargetPressureAt(7.9f),  4.0f, "step 1 held to its end");
  checkNear(profileTargetPressureAt(8),     6.0f, "step 2 begins");
  checkNear(profileTargetPressureAt(19.9f), 6.0f, "step 2 held flat, no ramp");
  checkNear(profileTargetPressureAt(35),    7.0f, "step 4 begins");

  std::printf("\nvertical edge (two points at one instant)\n");
  check(loadFromText("point,0,3\npoint,10,3\npoint,10,9\npoint,25,9\n"), "parses");
  checkNear(profileTargetPressureAt(9.9f), 3.0f, "before the edge");
  checkNear(profileTargetPressureAt(10),   9.0f, "at the edge the later point wins");

  // ── Rejections ─────────────────────────────────────────────────────────
  std::printf("\nmalformed input\n");
  check(!loadFromText("name,X\npoint,10,6\npoint,4,9\n"), "rejects out-of-order points");
  check(!loadFromText("name,X\npoint,0\n"),                "rejects a point missing its pressure");
  check(!loadFromText("name,X\npoint,0,6\n"),              "rejects a single-point curve");
  check(loadFromText("name,X\nwibble,1,2\npoint,0,3\npoint,10,9\n"), "ignores unknown keys");
  std::string tooMany;
  for (int i = 0; i <= MAX_PROFILE_POINTS; i++) tooMany += "point," + std::to_string(i) + ",9\n";
  check(!loadFromText(tooMany), "rejects more points than the firmware holds");
  check(!loadFromText("name," + std::string(200, 'x') + "\npoint,0,3\npoint,9,6\n"),
        "rejects a line longer than the parser's buffer");

  std::printf("\nclamping\n");
  check(loadFromText("name,X\npoint,0,99\npoint,9999,-4\n"), "parses out-of-range values");
  checkNear(profilePoints[0].bar, PROFILE_MAX_BAR, "pressure clamped to the ceiling");
  checkNear(profilePoints[1].bar, 0.0f, "negative pressure clamped to zero");
  checkNear(profilePoints[1].seconds, PROFILE_MAX_SECONDS, "time clamped to the ceiling");

  // ── Every shipped example recipe loads ─────────────────────────────────
  std::printf("\nshipped example recipes\n");
  const char *examples[] = {"ethiopia_guji", "house_blend", "decaf"};
  for (const char *name : examples) {
    std::ifstream f(std::string("sd_card_examples/recipes/") + name + ".csv");
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

  // ── Recipe names become file names ─────────────────────────────────────
  std::printf("\nrecipe name sanitising\n");
  char stem[RECIPE_NAME_MAX_LEN];
  check(sanitizeRecipeStem("ethiopia_guji", stem, sizeof(stem)), "accepts a normal name");
  check(std::strcmp(stem, "ethiopia_guji") == 0, "passes it through unchanged");
  check(sanitizeRecipeStem("Decaf-2", stem, sizeof(stem)), "accepts digits, caps and a dash");
  check(!sanitizeRecipeStem("../../etc/passwd", stem, sizeof(stem)), "rejects path traversal");
  check(!sanitizeRecipeStem("my recipe", stem, sizeof(stem)), "rejects spaces");
  check(!sanitizeRecipeStem("shot.csv", stem, sizeof(stem)), "rejects a dot");
  check(!sanitizeRecipeStem("a,b", stem, sizeof(stem)), "rejects a comma");
  check(!sanitizeRecipeStem("", stem, sizeof(stem)), "rejects an empty name");
  check(!sanitizeRecipeStem("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", stem, sizeof(stem)),
        "rejects a name with no room left for .csv");

  // ── Recipes ────────────────────────────────────────────────────────────
  std::printf("\nrecipe parsing\n");
  const char *recipe =
    "# marax recipe v1\n"
    "name,Ethiopia Guji\n"
    "grind,2,4\n"
    "dose,18.0\n"
    "yield,40.0\n"
    "temp,II\n"
    "point,0.0,3.0,ramp\n"
    "point,8.0,9.0,jump\n"
    "point,26.0,6.0,ramp\n";
  loadFromText("name,Loaded\npoint,0,4\npoint,20,9\n");
  uint32_t signatureBefore = profileSegmentSignature();
  resetRecipeScratch("ethiopia_guji");
  check(validateRecipeBody(recipe), "a full recipe validates");
  check(std::strcmp(recipeScratch.stem, "ethiopia_guji") == 0, "stem kept");
  check(std::strcmp(recipeScratch.name, "Ethiopia Guji") == 0, "name read, spaces and all");
  check(std::strcmp(recipeScratch.grind, "2,4") == 0, "grind runs to the end of the line, comma included");
  checkNear(recipeScratch.dose, 18.0f, "dose");
  checkNear(recipeScratch.yield, 40.0f, "yield");
  check(recipeScratch.temp == 2, "temp II is switch position 2");
  check(profileSegmentSignature() == signatureBefore, "validating leaves the pump's curve alone");

  // The recipe's curve means exactly what the same lines mean in a profile.
  profilePointCount = 0;
  resetRecipeScratch("ethiopia_guji");
  check(parseBodyLines(recipe), "parses into the live curve");
  check(profilePointCount == 3, "3 points");
  checkNear(profileTargetPressureAt(7.9f), 3.0f, "jump holds the previous pressure");
  checkNear(profileTargetPressureAt(8.0f), 9.0f, "...then steps");
  checkNear(profileTargetPressureAt(17.0f), 7.5f, "ramp interpolates");

  resetRecipeScratch("bare");
  check(validateRecipeBody("point,0,3\npoint,10,9\n"), "a recipe that is only a curve is valid");
  check(std::strcmp(recipeScratch.name, "bare") == 0, "...and is named after its file");
  check(recipeScratch.temp == RECIPE_TEMP_UNSET, "...with no temperature");
  checkNear(recipeScratch.yield, 0.0f, "...and no yield");

  resetRecipeScratch("x");
  check(!validateRecipeBody("name,X\ndose,18\nyield,36\n"), "rejects a recipe with no curve");
  resetRecipeScratch("x");
  check(!validateRecipeBody("name,X\npoint,10,6\npoint,4,9\n"), "rejects out-of-order points");
  resetRecipeScratch("x");
  check(validateRecipeBody("\xEF\xBB\xBFname,BOM\r\npoint,0,3\r\npoint,9,6\r\n"), "accepts a BOM and CRLF");
  check(std::strcmp(recipeScratch.name, "BOM") == 0, "...and still reads the first line");

  resetRecipeScratch("x");
  validateRecipeBody("name,  Padded  \ngrind,  3 \ndose,99\nyield,-5\npoint,0,3\npoint,9,6\n");
  check(std::strcmp(recipeScratch.name, "Padded") == 0, "name trimmed");
  check(std::strcmp(recipeScratch.grind, "3") == 0, "grind trimmed");
  checkNear(recipeScratch.dose, RECIPE_DOSE_MAX_G, "dose clamped to its ceiling");
  checkNear(recipeScratch.yield, 0.0f, "negative yield clamped to zero");

  resetRecipeScratch("x");
  validateRecipeBody("name,Say \"hi\"\\now\npoint,0,3\npoint,9,6\n");
  check(std::strcmp(recipeScratch.name, "Say 'hi'/now") == 0, "quotes and backslashes cannot reach the display");

  std::printf("\ntemperature switch\n");
  check(parseRecipeTemp("0") == 0 && parseRecipeTemp("I") == 1 && parseRecipeTemp("II") == 2,
        "0 / I / II as printed on the machine");
  check(parseRecipeTemp("1") == 1 && parseRecipeTemp("2") == 2, "digits too");
  check(parseRecipeTemp("III") == RECIPE_TEMP_UNSET && parseRecipeTemp("") == RECIPE_TEMP_UNSET,
        "anything else is not set");
  check(std::strcmp(recipeTempLabel(2), "II") == 0 && std::strcmp(recipeTempLabel(-1), "-") == 0,
        "labels round-trip");

  std::printf("\nhome tiles\n");
  check(parseHomeSlots("guji,house,decaf\n"), "three tiles");
  check(std::strcmp(homeSlotScratch[0], "guji") == 0 && std::strcmp(homeSlotScratch[2], "decaf") == 0,
        "in order");
  check(parseHomeSlots(",house,"), "unused tiles are empty entries");
  check(homeSlotScratch[0][0] == '\0' && std::strcmp(homeSlotScratch[1], "house") == 0 &&
        homeSlotScratch[2][0] == '\0', "...and stay empty");
  check(parseHomeSlots(""), "no tiles at all");
  check(parseHomeSlots("guji"), "fewer than three entries");
  check(!parseHomeSlots("a,b,c,d"), "rejects a fourth tile");
  check(!parseHomeSlots("../x,,"), "rejects a name that is not a file name");
  check(!parseHomeSlots("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), "rejects an over-long name");

  std::printf("\nhome tile text\n");
  char grams[12];
  formatGrams(18.0f, grams, sizeof(grams));
  check(std::strcmp(grams, "18") == 0, "whole grams without a decimal");
  formatGrams(18.5f, grams, sizeof(grams));
  check(std::strcmp(grams, "18.5") == 0, "half grams with one");
  char tile[128];
  resetRecipeScratch("guji");
  validateRecipeBody(recipe);
  buildHomeTileText(recipeScratch, tile, sizeof(tile));
  check(std::strcmp(tile, "Ethiopia Guji\\r18g > 40g\\rGrind 2,4\\rTemp II") == 0, "full tile");
  std::printf("        %s\n", tile);
  resetRecipeScratch("bare");
  validateRecipeBody("yield,36\npoint,0,3\npoint,9,6\n");
  buildHomeTileText(recipeScratch, tile, sizeof(tile));
  check(std::strcmp(tile, "bare\\rOut 36g") == 0, "lines for missing values are left out");
  resetRecipeScratch("aaaaaaaaaaaaaaaaaaaaaaaaaa");
  validateRecipeBody("name,A name that fills all 31 chars\ngrind,fifteen chars!!\ndose,18.5\n"
                     "yield,150\ntemp,II\npoint,0,3\npoint,9,6\n");
  buildHomeTileText(recipeScratch, tile, sizeof(tile));
  check(std::strlen(tile) < sizeof(tile) - 1, "the longest possible tile fits");

  std::printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
  return failures != 0;
}

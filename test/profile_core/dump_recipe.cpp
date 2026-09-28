// Reads a recipe on stdin the way loadRecipe() does and prints what the
// firmware made of it: the recipe fields, then its target pressure at regular
// intervals. Used by test/card/run.sh to check that the recipe card reads a
// recipe exactly as the machine does.
#include "profile_core.h"
#include <iostream>
#include <sstream>

int main() {
  std::stringstream buffer;
  buffer << std::cin.rdbuf();
  std::string text = buffer.str();

  profilePointCount = 0;
  resetRecipeScratch("recipe");
  if (!parseBodyLines(text.c_str()) || profilePointCount < PROFILE_MIN_POINTS) {
    std::printf("rejected\n");
    return 1;
  }

  std::printf("name=%s\n", recipeScratch.name);
  std::printf("grind=%s\n", recipeScratch.grind);
  std::printf("dose=%.1f\n", recipeScratch.dose);
  std::printf("yield=%.1f\n", recipeScratch.yield);
  std::printf("temp=%s\n", recipeScratch.temp == RECIPE_TEMP_UNSET ? "" : recipeTempLabel(recipeScratch.temp));
  std::printf("---\n");

  for (float t = 0.0f; t <= profileTotalSeconds() + 10.0f; t += 0.25f) {
    std::printf("%.2f %.4f\n", t, profileTargetPressureAt(t));
  }
  return 0;
}

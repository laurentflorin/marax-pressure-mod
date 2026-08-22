// Prints the firmware's target pressure at regular intervals for a profile
// given on stdin. Used by test/card/run.sh to check that the Lovelace card
// draws the same curve the pump actually follows.
#include "profile_core.h"
#include <iostream>
#include <sstream>

int main() {
  std::stringstream buffer;
  buffer << std::cin.rdbuf();
  std::istringstream in(buffer.str());

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
    std::string rowRaw;
    std::getline(in, rowRaw);
    String row(rowRaw);
    row.trim();
    if (!parseLegacyProfileRow(row)) return 1;
  } else {
    String line = header;
    while (line.length() > 0) {
      line.trim();
      if (line.length() > 0 && line.charAt(0) != '#') {
        char buf[PROFILE_CSV_BUFFER_LEN];
        line.toCharArray(buf, sizeof(buf));
        if (!parseProfileV2Line(buf)) return 1;
      }
      std::string next;
      if (!std::getline(in, next)) break;
      line = String(next);
    }
    if (profilePointCount < PROFILE_MIN_POINTS) return 1;
  }

  // The v2 body the firmware would hand to the card. For a legacy file this is
  // the converted form, which is the only thing the card ever sees.
  String body;
  buildProfileCsv(body);
  std::printf("%s---\n", body.s.c_str());

  for (float t = 0.0f; t <= profileTotalSeconds() + 10.0f; t += 0.25f) {
    std::printf("%.2f %.4f\n", t, profileTargetPressureAt(t));
  }
  return 0;
}

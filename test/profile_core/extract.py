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

globals_block = src[src.index("#define MAX_PROFILE_POINTS"):src.index("// SD and display globals")]
# The recipe defines, struct and globals, up to the MQTT command queue.
recipe_block = src[src.index("#define MAX_RECIPES"):src.index("// Kept here rather than up with the other MQTT globals")]

funcs = [
    grab(r"^static bool parsePointLine"),
    grab(r"^float profileTotalSeconds"),
    grab(r"^float profileTargetPressureAt"),
    grab(r"^uint32_t profileSegmentSignature"),
    grab(r"^int pressureToWave"),
    grab(r"^static bool sanitizeRecipeStem"),
    grab(r"^static int parseRecipeTemp"),
    grab(r"^static const char \*recipeTempLabel"),
    grab(r"^static void resetRecipeScratch"),
    grab(r"^static void copyTrimmedValue"),
    grab(r"^static bool parseRecipeLine"),
    grab(r"^static bool parseBodyLines"),
    grab(r"^static bool validateRecipeBody"),
    grab(r"^static bool parseHomeSlots"),
    grab(r"^static void formatGrams"),
    grab(r"^static void buildHomeTileText"),
]

prelude = '''#include <string>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <algorithm>\n#include <cmath>

#define RECIPE_NAME_MAX_LEN 32
#define RECIPE_LINE_MAX_LEN 128
#define BREW_WAVEFORM_HEIGHT 180
#define RECIPE_BODY_MAX_LEN 1024

template <typename T> T constrain(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

struct String {
  std::string s;
  String() {}
  String(const std::string &v) : s(v) {}
  bool startsWith(const char *p) const { return s.rfind(p, 0) == 0; }
  size_t length() const { return s.size(); }
  char charAt(size_t i) const { return s[i]; }
  void trim() {
    size_t a = s.find_first_not_of(" \\t\\r\\n");
    size_t b = s.find_last_not_of(" \\t\\r\\n");
    s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
  }
  String(const char *c) : s(c) {}
  String(float v, int digits) { char b[32]; std::snprintf(b, sizeof(b), "%.*f", digits, v); s = b; }
  String &operator+=(const String &o) { s += o.s; return *this; }
  String &operator+=(const char *c) { s += c; return *this; }
  void remove(size_t i, size_t n) { s.erase(i, n); }\n  void toCharArray(char *buf, size_t n) const { std::snprintf(buf, n, "%s", s.c_str()); }
};

struct FakeSerial {
  void print(const char *c) { std::fputs(c, stdout); }
  void print(int v) { std::printf("%d", v); }
  void print(float v, int d) { std::printf("%.*f", d, v); }
  void println(const char *c) { std::printf("%s\\n", c); }
  void println(int v) { std::printf("%d\\n", v); }
} Serial;

// Mirrors the forward declarations the .ino carries.\nfloat profileTotalSeconds();\nfloat profileTargetPressureAt(float second);\nint pressureToWave(float bar);

'''

open(sys.argv[1], "w").write(prelude + globals_block + "\n" + recipe_block + "\n" + "\n".join(funcs))
print("extracted %d functions" % len(funcs))

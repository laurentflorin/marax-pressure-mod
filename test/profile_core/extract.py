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

globals_block = src[src.index("#define MAX_PROFILE_POINTS"):src.index("// SD / Profile globals")]

funcs = [
    grab(r"^static void setActiveProfileName"),
    grab(r"^static bool isLegacyProfileHeader"),
    grab(r"^static bool parseLegacyProfileRow"),
    grab(r"^static bool parseProfileV2Line"),
    grab(r"^float profileTotalSeconds"),
    grab(r"^float profileTargetPressureAt"),
    grab(r"^void applyManualStepsToProfile"),
    grab(r"^uint32_t profileSegmentSignature"),
    grab(r"^int pressureToWave"),
    grab(r"^static bool sanitizeProfileStem"),
    grab(r"^void buildProfileCsv"),
    grab(r"^static bool validateProfileBody"),
]

prelude = '''#include <string>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <algorithm>\n#include <cmath>

#define PROFILE_NAME_MAX_LEN 32
#define PROFILE_CSV_BUFFER_LEN 128
#define PROFILE_FIELD_COUNT 10
#define BREW_WAVEFORM_HEIGHT 180

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

char activeProfileName[PROFILE_NAME_MAX_LEN];\n\n// Mirrors the forward declarations the .ino carries.\nvoid applyManualStepsToProfile();\nfloat profileTotalSeconds();\nfloat profileTargetPressureAt(float second);\nint pressureToWave(float bar);

'''

open(sys.argv[1], "w").write(prelude + globals_block + "\n" + "\n".join(funcs))
print("extracted %d functions" % len(funcs))

# -*- coding: utf-8 -*-
"""Lifts the Home Assistant discovery block out of the sketch for host testing."""
import io, re, sys

src = io.open("src/marax_esp32s3/marax_esp32s3.ino", encoding="utf-8").read()

defines = re.search(r'#define brewtemp_topic.*?#define PROFILE_SELECT_MAX_OPTIONS \d+', src, re.S).group(0)
block = re.search(r'#if !defined\(DISABLE_WIFI_MQTT\) && defined\(ENABLE_HA_DISCOVERY\)\n(.*?)\n#endif\n',
                  src, re.S).group(1)

prelude = '''#include <string>
#include <cstdio>
#include <cstring>
#include <algorithm>

#define PROFILE_NAME_MAX_LEN 32
#define MAX_PROFILES 64

struct String {
  std::string s;
  String() {}
  String(const char *c) : s(c) {}
  String(const std::string &v) : s(v) {}
  String &operator+=(const char *c) { s += c; return *this; }
  String &operator+=(const String &o) { s += o.s; return *this; }
  String operator+(const char *c) const { String r(*this); r.s += c; return r; }
  String operator+(const String &o) const { String r(*this); r.s += o.s; return r; }
  bool endsWith(const char *c) const {
    size_t n = std::strlen(c);
    return s.size() >= n && s.compare(s.size() - n, n, c) == 0;
  }
  String substring(size_t a, size_t b) const { return String(s.substr(a, b - a)); }
  size_t length() const { return s.size(); }
  const char *c_str() const { return s.c_str(); }
};
static String operator+(const char *c, const String &o) { return String(c) + o; }

static size_t maxFrame = 0;
static int overSized = 0;

struct FakeSerial {
  void print(const char *c) { std::fputs(c, stdout); }
  void print(int v) { std::printf("%d", v); }
  void print(size_t v) { std::printf("%zu", v); }
  void println(const char *c) { std::printf("%s\\n", c); }
  void println(int v) { std::printf("%d\\n", v); }
} Serial;

struct FakeClient {
  bool publish(const char *topic, const char *payload, bool retained) {
    size_t frame = 5 + 2 + std::strlen(topic) + std::strlen(payload);
    maxFrame = std::max(maxFrame, frame);
    if (frame > MQTT_BUFFER_SIZE) overSized++;
    std::printf("  %-56s frame=%4zu %s\\n", topic, frame,
                frame > MQTT_BUFFER_SIZE ? "OVER BUFFER" : "");
    return frame <= MQTT_BUFFER_SIZE;
  }
} mqttClient;

char profileNames[MAX_PROFILES][PROFILE_NAME_MAX_LEN];
int profileCount = 0;

'''

open(sys.argv[1], "w").write("#include <cstdint>\n" + defines + "\n\n" + prelude + block)
print("extracted discovery block")

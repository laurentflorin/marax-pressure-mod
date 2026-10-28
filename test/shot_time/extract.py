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

# The thresholds and the evidence state are the thing under test, so they are
# lifted from the sketch rather than restated here.
state = span("#define SHOT_TIME_MIN_SECONDS", "float shotTimeToRemember")

prelude = '''#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>

// ── Sketch state the checks read ──────────────────────────────────────────
// The test sets the scale up directly; nothing else is touched.
static bool scaleConnected = false;
static unsigned long lastWeightTime = 0;
static float currentWeight = 0.0f;
'''

out = sys.argv[1]
io.open(out, "w", encoding="utf-8").write(
    prelude + "\n" + state + "\n"
    + grab(r"^void startShotEvidence")
    + grab(r"^void noteShotWeight")
    + grab(r"^const char \*shotTimeRejection"))
print("extracted the shot-time checks out of the sketch")

# -*- coding: utf-8 -*-
"""Reproduces the prototype block the Arduino builder injects into a .ino.

Without it every function used before its definition looks undeclared, which
buries the errors that are actually real.
"""
import io, re, sys

src = io.open(sys.argv[1], encoding="utf-8").read()

DEFINITION = re.compile(
    r'^((?:\w[\w:]*\s+)+'                 # return type, possibly several words
    r'\**\s*'                             # pointer, if any
    r'\w+\s*'                             # name
    r'\([^)]*\))\s*\n\{',                # parameter list, then a brace on its own line
    re.M)

prototypes = []
for match in DEFINITION.finditer(src):
    signature = " ".join(match.group(1).split())
    if re.match(r'^(if|for|while|switch|return|else)\b', signature):
        continue
    prototypes.append(signature + ";")

# Arduino inserts the block just before the first function definition, so
# everything declared above it — macros, types, globals — is already in scope.
first = DEFINITION.search(src)
block = "\n// --- generated prototypes (mirrors the Arduino builder) ---\n" + \
        "\n".join(prototypes) + "\n// --- end generated prototypes ---\n\n"
io.open(sys.argv[2], "w", encoding="utf-8").write(src[:first.start()] + block + src[first.start():])
sys.stderr.write("injected %d prototypes\n" % len(prototypes))

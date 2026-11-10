#!/usr/bin/env python3
"""Assembles each case's text with llvm-mc and compares the bytes."""
import re, subprocess, sys

lines = [l.rstrip("\n").split("|") for l in open(sys.argv[1])]
fails = 0
# One llvm-mc run per case keeps the mapping simple (cases can hold several instructions).
asm = "\n".join(f".Lcase{i}:\n" + "\n".join(t.strip() for t in text.split(";"))
                for i, (text, *_ ) in enumerate(lines))
res = subprocess.run(["llvm-mc", "--triple=thumbv7-linux-gnueabihf", "-mattr=+vfp3,+neon,+hwdiv",
                      "--show-encoding"], input=".syntax unified\n.thumb\n" + asm,
                     capture_output=True, text=True)
if res.returncode != 0:
    print(res.stderr[:3000]); sys.exit(1)
cur, theirs = -1, {}
for l in res.stdout.splitlines():
    m = re.match(r"\.Lcase(\d+):", l.strip())
    if m:
        cur = int(m.group(1)); theirs[cur] = ""; continue
    m = re.search(r"encoding: \[([^\]]*)\]", l)
    if m and cur >= 0:
        theirs[cur] += "".join(b[2:] for b in m.group(1).split(","))
for i, parts in enumerate(lines):
    text, ours = parts[0], parts[1]
    flag = parts[2] if len(parts) > 2 else ""
    if ours != theirs.get(i) or flag:
        fails += 1
        print(f"FAIL {text}\n   ours {ours} {flag}\n   llvm {theirs.get(i)}")
print(f"encodings: {len(lines) - fails}/{len(lines)} match llvm-mc")
sys.exit(1 if fails else 0)

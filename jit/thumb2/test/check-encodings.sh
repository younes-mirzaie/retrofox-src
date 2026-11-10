#!/bin/sh
# Compares every encoding Thumb2Assembler.h produces with llvm-mc's, for the
# cases gen_cases.py generates. Needs clang++, llvm-mc, python3.
set -e
cd "$(dirname "$0")"
OUT=${OUT:-/tmp/thumb2-enc}; mkdir -p "$OUT"
python3 gen_cases.py "$OUT/cases.inc" "${SEED:-1}"
clang++ -std=c++14 -O1 -Wall -I.. -I"$OUT" encode_test.cpp -o "$OUT/encode_test"
"$OUT/encode_test" > "$OUT/ours.txt"
python3 compare.py "$OUT/ours.txt"

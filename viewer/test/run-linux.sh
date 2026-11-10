#!/bin/sh
# Builds rfxview-demo for Linux and runs the protocol test against it.
set -e
cd "$(dirname "$0")/.."
OUT=${OUT:-/tmp/rfxview-test}; mkdir -p "$OUT"
${CXX:-g++} -std=c++14 -O2 -Wall -Wextra -Wno-unused-parameter engine/rfxview.cpp engine/rfxview-demo.cpp -o "$OUT/rfxview-demo"
python3 test/test_protocol.py "$OUT/rfxview-demo"

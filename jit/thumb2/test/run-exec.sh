#!/bin/sh
# Builds exec_test.cpp for Linux ARM hard-float and runs it under qemu-arm.
set -e
cd "$(dirname "$0")"
OUT=${OUT:-/tmp/thumb2-exec}; mkdir -p "$OUT"
clang++ --target=armv7a-linux-gnueabihf -mthumb -mfloat-abi=hard -mfpu=neon -O2 -std=c++14 \
  -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector -fomit-frame-pointer \
  -fno-threadsafe-statics -nostdlib -I.. -c exec_test.cpp -o "$OUT/exec_test.o"
ld.lld -static -e _start -o "$OUT/exec_test" "$OUT/exec_test.o"
qemu-arm-static -cpu cortex-a15 "$OUT/exec_test"

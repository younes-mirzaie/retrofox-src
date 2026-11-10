#!/bin/sh
# Builds the xptcall test for Linux ARM hard-float (same AAPCS-VFP rules as
# Windows on ARM; char is signed there, hence -fsigned-char) and runs it under qemu-arm. Needs clang, ld.lld, qemu-user.
set -e
cd "$(dirname "$0")"
OUT=${OUT:-/tmp/xptcall-test}; mkdir -p "$OUT"
python3 gen.py "$OUT/tests.inc"
CXX="clang++ --target=armv7a-linux-gnueabihf -mthumb -mfloat-abi=hard -mfpu=neon"
FLAGS="-O${OPT:-2} -std=c++14 -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector -fsigned-char -fshort-wchar -DXPTC_ARM_WIN_TEST -I. -I.. -I$OUT -Wall -Wno-unused-function"
for f in ../xptcinvoke_arm_win.cpp ../xptcstubs_arm_win.cpp test_xptcall.cpp runtime.cpp; do
  EXTRA=; [ "$f" = runtime.cpp ] && EXTRA="-O2 -fomit-frame-pointer"  # svc needs r7
  $CXX $FLAGS $EXTRA -c "$f" -o "$OUT/$(basename "$f" .cpp).o"
done
ld.lld -static -e _start -o "$OUT/xptcall-test" "$OUT"/*.o
qemu-arm-static "$OUT/xptcall-test"

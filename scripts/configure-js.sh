#!/bin/bash
# Configures UXP's SpiderMonkey (js/src) as a Windows Phone 8.1 ARM (Thumb-2) shell, interpreter only.
# Run inside MozillaBuild:  C:\mozilla-build\msys2\usr\bin\bash.exe -l /c/rfx/retrofox/scripts/configure-js.sh
set -e
source /c/rfx/retrofox/scripts/phone-env.sh
OBJ=/c/rfx/obj-js
generate_configure "$SRC/js/src"

mkdir -p "$OBJ"
cd "$OBJ"
# js/src/configure is generated; this is what it runs
export OLD_CONFIGURE="$SRC/js/src/old-configure"
python3 "$SRC/configure.py" --enable-project=js \
  --host=x86_64-pc-mingw32 \
  --target=arm-pc-mingw32 \
  --disable-ion \
  --disable-ctypes \
  --disable-tests \
  --disable-jemalloc \
  --disable-shared-js \
  --enable-optimize \
  --disable-debug \
  "$@"

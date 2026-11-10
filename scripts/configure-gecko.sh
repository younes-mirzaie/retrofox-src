#!/bin/bash
# Configures the full UXP engine (application: xulrunner) for Windows Phone 8.1 ARM.
# Browser step 3, milestone 1: get dom/layout/gfx/netwerk compiling for the phone.
# Run inside MozillaBuild:  C:\mozilla-build\msys2\usr\bin\bash.exe -l /c/rfx/retrofox/scripts/configure-gecko.sh
#
# Off for now: anything needing Win32 desktop APIs or extra toolchains the phone build lacks
# (MIDL for accessibility, yasm-only media codecs, NPAPI plugins, WebRTC, updater/crash reporter),
# plus the JIT (SpiderMonkey's ARM JIT emits A32 code; Windows on ARM wants Thumb-2).
set -e
source /c/rfx/retrofox/scripts/phone-env.sh
OBJ=/c/rfx/obj-gecko

generate_configure "$SRC"
generate_configure "$SRC/js/src"

mkdir -p "$OBJ"
cd "$OBJ"
export OLD_CONFIGURE="$SRC/old-configure"
python3 "$SRC/configure.py" \
  --enable-application=xulrunner \
  --enable-windows-phone \
  --enable-fmp4 \
  --disable-shared-js \
  --host=x86_64-pc-mingw32 \
  --target=arm-pc-mingw32 \
  --enable-optimize \
  --disable-debug \
  --disable-tests \
  --disable-jemalloc \
  --disable-ion \
  --disable-ctypes \
  --disable-accessibility \
  --disable-npapi \
  --disable-webrtc \
  --disable-av1 \
  --disable-ffmpeg \
  --disable-wmf \
  --disable-printing \
  --disable-webspeech \
  --disable-updater \
  --disable-crashreporter \
  --disable-maintenance-service \
  --disable-parental-controls \
  --disable-negotiateauth \
  --disable-necko-wifi \
  --disable-gamepad \
  --disable-devtools-server \
  --disable-precompiled-startupcache \
  --disable-mapi \
  "$@"

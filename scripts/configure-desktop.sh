#!/bin/bash
# Configures the same engine as configure-gecko.sh (widget/retrofox, the phone's prefs and
# features) for desktop Windows x86, in C:\rfx\obj-desktop. rfxengine.exe from it serves the
# Retrofox app on port 8081 like the phone's, so the app (phone, or the emulator through the
# hidden "engine address" option) and page problems can be tested from the PC.
# Run inside MozillaBuild:  C:\mozilla-build\msys2\usr\bin\bash.exe -l /c/rfx/retrofox/scripts/configure-desktop.sh
set -e
source /c/rfx/retrofox/scripts/phone-env.sh
OBJ=/c/rfx/obj-desktop

# Desktop headers and libraries: the Windows 10 SDK (22621) and the VS 2022 x86 CRT. A current
# PC's instruction set (AVX2, AES, PCLMUL): clang, unlike MSVC, only compiles intrinsics the target
# has, and qcms and NSS's AES-GCM use SSE2 to SSSE3, AES-NI and PCLMUL ones. This build only runs
# on the PC, for testing.
TARGET_FLAGS="--target=i686-pc-windows-msvc -X \
 -imsvc $VC14W/include -imsvc $UCRTW/Include/$UCRTVER/ucrt \
 -imsvc $UCRTW/Include/$UCRTVER/um -imsvc $UCRTW/Include/$UCRTVER/shared"
export CC="clang-cl $TARGET_FLAGS -fms-compatibility-version=19.44.35207"
# (configure strips -arch from CC itself, so the instruction set goes in the flags)
ISA="-arch:AVX2 -clang:-maes -clang:-mpclmul"
export CFLAGS="-Wno-incompatible-pointer-types -D_CRT_RAND_S $ISA"
export CXXFLAGS="-Wno-c++11-narrowing -D_CRT_RAND_S $ISA"
export CXX="clang-cl $TARGET_FLAGS -fms-compatibility-version=19.44.35207 -std:c++17"
# Full paths: for x86, configure looks for the linker only in Visual Studio's own folders
export LINKER="C:/Program Files/LLVM/bin/lld-link.exe"
export LD="C:/Program Files/LLVM/bin/lld-link.exe"
export AR=llvm-lib
export LIB="$VC14W/lib/x86;$UCRTW/Lib/$UCRTVER/ucrt/x86;$UCRTW/Lib/$UCRTVER/um/x86"
export LDFLAGS="-fuse-ld=lld -errorlimit:0"
export HOST_CC="clang-cl -fms-compatibility-version=19.44.35207"
export HOST_CXX="clang-cl -fms-compatibility-version=19.44.35207 -std:c++17"
export HOST_LDFLAGS="-LIBPATH:$VC14W/lib/x64 -LIBPATH:$UCRTW/Lib/$UCRTVER/ucrt/x64 -LIBPATH:$UCRTW/Lib/$UCRTVER/um/x64"
export INCLUDE="$UCRTW/Include/$UCRTVER/um;$UCRTW/Include/$UCRTVER/shared;$UCRTW/Include/$UCRTVER/ucrt;$VC14W/include"
export MSYS2_ENV_CONV_EXCL="HOST_LDFLAGS;INCLUDE;LIB"

mkdir -p "$OBJ"
cd "$OBJ"
export OLD_CONFIGURE="$SRC/old-configure"
# The same switches as the phone (configure-gecko.sh), so pages behave the same
python3 "$SRC/configure.py" \
  --enable-application=xulrunner \
  --enable-windows-phone \
  --enable-fmp4 \
  --disable-shared-js \
  --host=x86_64-pc-mingw32 \
  --target=i686-pc-mingw32 \
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

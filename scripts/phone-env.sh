#!/bin/bash
# Shared environment for building UXP code for Windows Phone 8.1 ARM (Thumb-2).
# Sourced by configure-js.sh and configure-gecko.sh, inside MozillaBuild bash.
# Paths go through the ASCII junctions in C:\rfx because Mozilla's build breaks on the em dash in the real path.

SRC=/c/rfx/uxp
RFX=/c/rfx/retrofox
LLVM="/c/Program Files/LLVM/bin"

# Space-free junctions (old autoconf word-splits $CC, so quoted paths don't survive):
#   C:\rfx\sdk\vc143    -> VS 2022 Build Tools VC\Tools\MSVC\14.44.35207 (ARM CRT + C++17 STL)
#   C:\rfx\sdk\ucrt     -> Windows Kits\10                   (UCRT 10.0.22621, last SDK with ARM)
#   C:\rfx\sdk\phonekit -> Windows Phone Kits\8.1            (what the phone allows)
#   C:\rfx\sdk\win81    -> Windows Kits\8.1                  (desktop 8.1 headers)
VC14W=C:/rfx/sdk/vc143
UCRTW=C:/rfx/sdk/ucrt
UCRTVER=10.0.22621.0
PHONEKITW=C:/rfx/sdk/phonekit
WIN81W=C:/rfx/sdk/win81

# UXP's rules call the MSVC librarian as plain "lib"; llvm-lib copied to lib.exe stands in
mkdir -p /c/rfx/tools
[ -f /c/rfx/tools/lib.exe ] || cp "$LLVM/llvm-lib.exe" /c/rfx/tools/lib.exe
# dumpbin -exports for the libxul module-order check (no MSVC tools here)
cp "$(dirname "${BASH_SOURCE[0]}")/tools/dumpbin" /c/rfx/tools/dumpbin
export PATH="/c/rfx/tools:$LLVM:/c/mozilla-build/python3:/c/mozilla-build/bin:$PATH"
# Static CRT everywhere: the phone has no UCRT/VCRUNTIME DLLs (see patches/README.md)
export RETROFOX_STATIC_RTL=1
# MozillaBuild 4 dropped mozmake; use native GNU make (winget ezwinports.make)
export GMAKE="$(cygpath -u "$LOCALAPPDATA")/Microsoft/WinGet/Links/make.exe"

# Target headers: desktop Windows 8.1 SDK, so everything the phone's DLLs actually export is
# declared. What really exists is decided at link time, against the phone's API set
# (WindowsPhoneCore.lib + scripts/phone-libs). -X ignores INCLUDE/auto-detected SDKs.
TARGET_FLAGS="--target=thumbv7-pc-windows-msvc -X \
 -I C:/rfx/retrofox/shim/include -FI retrofox-phone.h \
 -D_ARM_WINAPI_PARTITION_DESKTOP_SDK_AVAILABLE=1 \
 -imsvc $VC14W/include -imsvc $UCRTW/Include/$UCRTVER/ucrt \
 -imsvc $WIN81W/Include/um -imsvc $WIN81W/Include/shared"

# UXP pins clang-cl to VS 2015 Update 3 compatibility; giving it C++17 up front stops configure
# from adding "-Xclang -std:c++17", which current clang rejects
export CC="clang-cl $TARGET_FLAGS -fms-compatibility-version=19.44.35207"
# NSPR's C code passes int* where Windows wants DWORD* (same size on ARM). configure drops
# warning switches from CC, so this goes through CFLAGS.
export CFLAGS="-Wno-incompatible-pointer-types -D_CRT_RAND_S"
# Code that MSVC accepts with a warning: D2D cairo and switch cases on unsigned values
export CXXFLAGS="-Wno-c++11-narrowing -D_CRT_RAND_S"
export CXX="clang-cl $TARGET_FLAGS -fms-compatibility-version=19.44.35207 -std:c++17"
export LINKER=lld-link
export LD=lld-link
export AR=llvm-lib

# kernel32.lib (phone DLL import lists), desktop-lib placeholders, retrofox-compat.lib
bash "$RFX/scripts/make-phone-libs.sh"

# clang-cl rejects linker switches in LDFLAGS, so library paths go through LIB (read by lld-link)
export LIB="$VC14W/lib/arm;$UCRTW/Lib/$UCRTVER/ucrt/arm;$PHONEKITW/lib/ARM;C:/rfx/retrofox/build/lib"
export LDFLAGS="-fuse-ld=lld -errorlimit:0 WindowsPhoneCore.lib retrofox-compat.lib"

# Host tools use the normal desktop toolchain clang-cl finds by itself. LIB (above) points at the
# ARM libraries, so host links name the x64 ones explicitly (-LIBPATH wins over LIB).
export HOST_CC="clang-cl -fms-compatibility-version=19.44.35207"
export HOST_CXX="clang-cl -fms-compatibility-version=19.44.35207 -std:c++17"
export HOST_LDFLAGS="-LIBPATH:$VC14W/lib/x64 -LIBPATH:$UCRTW/Lib/$UCRTVER/ucrt/x64 -LIBPATH:$UCRTW/Lib/$UCRTVER/um/x64"
# The resource compiler (rc) reads INCLUDE (clang-cl ignores it because of -X)
export INCLUDE="$WIN81W/Include/um;$WIN81W/Include/shared;$UCRTW/Include/$UCRTVER/ucrt;$VC14W/include"
# Keep MSYS from "converting" these as POSIX path lists ("-LIBPATH:C:/x" looked like one)
export MSYS2_ENV_CONV_EXCL="HOST_LDFLAGS;INCLUDE;LIB"

# js/src/configure and the top-level configure aren't in the repo, but make insists they're newer
# than configure.in/old-configure.in (which our patches touch). Each is configure.in minus the m4
# divert line. Only the configure-*.sh scripts call this, never builds (make would see them change).
generate_configure() {
  sed '/^: "divert(0)dnl"$/d' "$1/configure.in" | sed '1,/^#!\/bin\/sh$/{/^#!\/bin\/sh$/!d}' > "$1/configure"
}

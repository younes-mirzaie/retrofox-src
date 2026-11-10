#!/bin/bash
# Builds the configured SpiderMonkey shell (run configure-js.sh first).
# Run inside MozillaBuild:  C:\mozilla-build\msys2\usr\bin\bash.exe -l /c/rfx/retrofox/scripts/build-js.sh [-jN] [make args]
set -e
LLVM="/c/Program Files/LLVM/bin"
export PATH="/c/rfx/tools:$LLVM:/c/mozilla-build/python3:$PATH"
# Static CRT everywhere: the phone has no UCRT/VCRUNTIME DLLs (see patches/README.md)
export RETROFOX_STATIC_RTL=1
export LIB="C:/rfx/sdk/vc143/lib/arm;C:/rfx/sdk/ucrt/Lib/10.0.22621.0/ucrt/arm;C:/rfx/sdk/phonekit/lib/ARM;C:/rfx/retrofox/build/lib"
MAKE="$(cygpath -u "$LOCALAPPDATA")/Microsoft/WinGet/Links/make.exe"

cd /c/rfx/obj-js
# No action_cl wrapper in cross builds (see patches/README.md)
"$MAKE" -j"$(nproc)" CC_WRAPPER= CXX_WRAPPER= "$@"

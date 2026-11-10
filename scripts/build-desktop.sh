#!/bin/bash
# Builds the desktop x86 engine configured by configure-desktop.sh (C:\rfx\obj-desktop).
# Run inside MozillaBuild:  C:\mozilla-build\msys2\usr\bin\bash.exe -l /c/rfx/retrofox/scripts/build-desktop.sh [make args]
# Not phone-env.sh: its compiler settings are the phone's, and a configure re-run from make
# would pick them up. None of build-gecko.sh's ARM fix-ups apply to x86.
set -e
export PATH="/c/rfx/tools:/c/Program Files/LLVM/bin:/c/mozilla-build/python3:/c/mozilla-build/bin:$PATH"
export RETROFOX_STATIC_RTL=1
# The resource compiler (rc) reads INCLUDE: the same desktop SDK headers as configure-desktop.sh
SDKINC=C:/rfx/sdk/ucrt/Include/10.0.22621.0
export INCLUDE="$SDKINC/um;$SDKINC/shared;$SDKINC/ucrt;C:/rfx/sdk/vc143/include"
export MSYS2_ENV_CONV_EXCL="INCLUDE"
GMAKE="$(cygpath -u "$LOCALAPPDATA")/Microsoft/WinGet/Links/make.exe"
cd /c/rfx/obj-desktop
"$GMAKE" -j"$(nproc)" CC_WRAPPER= CXX_WRAPPER= \
  '_DEPEND_CFLAGS=-clang:-MD -clang:-MP -clang:-MF$(MDDEPDIR)/$(@F).pp' "$@"
# The install manifests leave symlinks in dist/bin (into the source tree); Gecko on Windows reads
# a symlink as an empty file (its size is the link's), so the engine gets empty JS modules and
# crashes. Make them copies.
find dist/bin -type l | while read -r link; do
  target="$(readlink -f "$link")"
  if [ -f "$target" ]; then
    rm "$link" && cp "$target" "$link" || echo "can't copy $target to $link"
  fi
done

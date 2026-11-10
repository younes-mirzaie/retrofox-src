#!/bin/bash
# Builds the configured UXP engine (run configure-gecko.sh first).
# Run inside MozillaBuild:  C:\mozilla-build\msys2\usr\bin\bash.exe -l /c/rfx/retrofox/scripts/build-gecko.sh [make args]
set -e
source /c/rfx/retrofox/scripts/phone-env.sh
cd /c/rfx/obj-gecko
# No action_cl wrapper in cross builds (see patches/README.md). That wrapper is what writes the
# .deps/*.pp files for MSVC-style compilers, so clang-cl writes them itself; without them make
# never rebuilds an object after a header (or a file inside a unified source) changes.
# (-clang: rather than /clang: so msys doesn't take it for a path.)
build() {
  "$GMAKE" -j"$(nproc)" CC_WRAPPER= CXX_WRAPPER= \
    '_DEPEND_CFLAGS=-clang:-MD -clang:-MP -clang:-MF$(MDDEPDIR)/$(@F).pp' "$@"
}
build "$@"
# clang's vcall thunks clobber r1 on 32-bit ARM: libxul links fixed copies ahead of them
# (scripts/vcall-thunks.py). After a normal build, make sure the list covers every thunk the
# objects now define, and relink if it changed.
if [ $# -eq 0 ]; then
  thunks=/c/rfx/uxp/toolkit/library/RfxVcallThunks.cpp
  before=$(md5sum "$thunks" 2>/dev/null)
  python3 /c/rfx/retrofox/scripts/vcall-thunks.py C:/rfx/obj-gecko "$(cygpath -m "$thunks")"
  [ "$before" = "$(md5sum "$thunks")" ] || build
fi
# lld writes "local import" pointers (dllimport calls into the same image) without the Thumb bit,
# so those calls run in ARM state and fault: set it in everything installed (fix-thumb-imports.py)
for d in dist/bin dist/xulrunner; do
  [ -d "$d" ] && python3 /c/rfx/retrofox/scripts/fix-thumb-imports.py "$(cygpath -m "$d")"
done
true

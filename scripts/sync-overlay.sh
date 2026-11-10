#!/bin/bash
# Copies retrofox's own Gecko sources into the UXP tree and hooks them into the build.
# Run after applying the patches, and again whenever these files change:
#   bash scripts/sync-overlay.sh [path/to/uxp]      (default: /c/rfx/uxp)
#
#   gecko/rfxengine/*              -> retrofox/rfxengine/    (rfxengine.exe, and lib/ in libxul)
#   viewer/engine/rfxview.{h,cpp}  -> retrofox/rfxengine/lib/ (app protocol)
#   gecko/xptcall/*_arm_win*       -> xpcom/reflect/xptcall/md/win32/
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
UXP="${1:-/c/rfx/uxp}"
[ -f "$UXP/moz.build" ] || { echo "not a UXP tree: $UXP"; exit 1; }

mkdir -p "$UXP/retrofox/rfxengine/lib"
# Before the engine moved into libxul these sat in retrofox/rfxengine itself
rm -f "$UXP"/retrofox/rfxengine/{rfxengine.cpp,rfxview.h,rfxview.cpp}
cp "$HERE"/gecko/rfxengine/{main.cpp,moz.build} "$UXP/retrofox/rfxengine/"
cp "$HERE"/gecko/rfxengine/lib/{rfxengine.cpp,moz.build} "$UXP/retrofox/rfxengine/lib/"
cp "$HERE"/viewer/engine/rfxview.{h,cpp} "$UXP/retrofox/rfxengine/lib/"
cp "$HERE"/gecko/xptcall/{xptc_arm_win_abi.h,xptcinvoke_arm_win.cpp,xptcstubs_arm_win.cpp} \
   "$UXP/xpcom/reflect/xptcall/md/win32/"

# toolkit/library builds this (patches/uxp-phone-link.patch); build-gecko.sh fills it in from
# the objects with vcall-thunks.py after each build, so a fresh tree only needs a placeholder
THUNKS="$UXP/toolkit/library/RfxVcallThunks.cpp"
[ -f "$THUNKS" ] || echo "/* Filled in by retrofox scripts/vcall-thunks.py */" > "$THUNKS"

# Build retrofox/rfxengine in phone builds (once)
if ! grep -q "retrofox/rfxengine" "$UXP/moz.build"; then
  cat >> "$UXP/moz.build" <<'HOOK'

# retrofox: the phone engine (scripts/sync-overlay.sh)
if CONFIG['MOZ_WINDOWS_PHONE']:
    DIRS += ['retrofox/rfxengine']
HOOK
  echo "hooked retrofox/rfxengine into moz.build"
fi
echo "overlay synced into $UXP"

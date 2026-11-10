#!/bin/bash
# Gathers the engine's files for the Retrofox test build with the engine inside the app
# (msbuild /p:InAppEngine=true) in C:\rfx\inapp-engine. They go at the package root, next to the
# app, so the loader finds xul.dll and its DLLs there. Libraries from the latest build (dist/bin),
# the rest (omni.ja, platform.ini, dictionaries) from the last package (dist/xulrunner).
set -e
OBJ=/c/rfx/obj-gecko/dist
OUT=/c/rfx/inapp-engine
rm -rf "$OUT"
mkdir -p "$OUT"
for f in xul.dll mozglue.dll nss3.dll freebl3.dll softokn3.dll nssckbi.dll icu78.dll gkmedias.dll \
         hunspell.dll lgpllibs.dll mozavcodec.dll mozavutil.dll mozsqlite3.dll; do
  cp "$OBJ/bin/$f" "$OUT/"
done
cp "$OBJ/xulrunner/omni.ja" "$OBJ/xulrunner/platform.ini" "$OBJ/xulrunner/chrome.manifest" \
   "$OBJ/xulrunner/dependentlibs.list" "$OUT/"
cp -r "$OBJ/xulrunner/dictionaries" "$OUT/"
ls "$OUT" | wc -l
du -sh "$OUT"

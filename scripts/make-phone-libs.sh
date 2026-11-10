#!/bin/bash
# Builds the link-time view of the phone into build/lib:
#   kernel32.lib         = every phone-libs/<DLL>.def merged (the CRT links "kernel32.lib" by default,
#                          and the phone has no kernel32.dll)
#   <desktop>.lib        = empty placeholders for desktop libraries the phone lacks; their functions
#                          resolve through kernel32.lib or retrofox-compat.lib instead
#   retrofox-compat.lib  = compat/*.c, missing functions built on ones the phone has
# Called by configure-js.sh and env.ps1. Run inside MozillaBuild bash.
set -e
# Hand tools plain Windows paths and keep MSYS from rewriting switches like -Fo or /MT
export MSYS2_ARG_CONV_EXCL="*"
RFX=C:/rfx/retrofox
LLVM="/c/Program Files/LLVM/bin"
OUT=$RFX/build/lib
TMP=$RFX/build/lib/tmp
mkdir -p "$(cygpath -u "$TMP")"

parts=()
for def in /c/rfx/retrofox/scripts/phone-libs/*.def; do
  dll=$(basename "$def" .def)
  "$LLVM/llvm-dlltool.exe" -m arm -d "$(cygpath -m "$def")" -l "$TMP/$dll.lib"
  parts+=("$TMP/$dll.lib")
done
"$LLVM/llvm-lib.exe" -nologo -out:"$OUT/kernel32.lib" "${parts[@]}"

for name in advapi32 user32 gdi32 wsock32 mswsock winmm dbghelp psapi shell32 ole32 version \
            secur32 crypt32 iphlpapi userenv shlwapi comctl32 imm32 ntdll comdlg32 winspool \
            usp10 msimg32 dwmapi locationapi msdmo netapi32 oleacc rasapi32 rasdlg sapi sensorsapi \
            setupapi uxtheme wininet wintrust wtsapi32 amstrmid dmoguids strmiids wbemuuid \
            wmcodecdspuuid; do
  printf 'LIBRARY %s.dll\nEXPORTS\n' "$name" > "$(cygpath -u "$TMP")/$name.empty.def"
  "$LLVM/llvm-dlltool.exe" -m arm -d "$TMP/$name.empty.def" -l "$OUT/$name.lib"
done

# GUID-only static libraries (data, no DLL behind them) that the phone kit lacks: the Windows 8.1
# SDK's ARM copies work as they are
for name in PortableDeviceGuids; do
  cp "C:/rfx/sdk/win81/Lib/winv6.3/um/arm/$name.lib" "$OUT/$(echo $name | tr A-Z a-z).lib"
done

objs=()
for src in /c/rfx/retrofox/scripts/compat/*.c; do
  obj="$TMP/$(basename "$src" .c).obj"
  "$LLVM/clang-cl.exe" --target=thumbv7-pc-windows-msvc -X /MT /O2 /c \
    /imsvc C:/rfx/sdk/vc143/include /imsvc C:/rfx/sdk/ucrt/Include/10.0.22621.0/ucrt \
    /imsvc C:/rfx/sdk/win81/Include/um /imsvc C:/rfx/sdk/win81/Include/shared \
    -D_ARM_WINAPI_PARTITION_DESKTOP_SDK_AVAILABLE=1 "/Fo$obj" "$(cygpath -m "$src")"
  objs+=("$obj")
done
"$LLVM/llvm-lib.exe" -nologo -out:"$OUT/retrofox-compat.lib" "${objs[@]}"

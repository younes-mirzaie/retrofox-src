# Shared paths and compiler flags for building native ARM (Thumb-2) programs for Windows Phone 8.1.
# Same setup as configure-js.sh: desktop Windows 8.1 headers, linked against the phone's API set.
# Dot-source it: . .\scripts\env.ps1
$Llvm      = "C:\Program Files\LLVM\bin"
$Sdk       = "C:\rfx\sdk"                 # junctions: vc143, ucrt, win81, phonekit (see configure-js.sh)
$UcrtVer   = "10.0.22621.0"
$Root      = Split-Path $PSScriptRoot -Parent
$BuildRoot = Join-Path $Root "build"
$PhoneLib  = Join-Path $BuildRoot "lib"   # kernel32.lib etc., made by configure-js.sh

$Target = "thumbv7-pc-windows-msvc"

$CFlags = @(
    "--target=$Target", "/MT", "/O2", "/W3", "-X",
    "-I", "$Root\shim\include", "-FI", "retrofox-phone.h",
    "-D_ARM_WINAPI_PARTITION_DESKTOP_SDK_AVAILABLE=1",
    "/imsvc", "$Sdk\vc143\include",
    "/imsvc", "$Sdk\ucrt\Include\$UcrtVer\ucrt",
    "/imsvc", "$Sdk\win81\Include\um",
    "/imsvc", "$Sdk\win81\Include\shared"
)

# kernel32.lib (phone DLL import lists), placeholders and retrofox-compat.lib; see make-phone-libs.sh
# (bash prints a harmless "stty" warning on stderr, which callers using -ErrorAction Stop would
# treat as fatal; only the exit code counts)
$rfxPrevEAP = $ErrorActionPreference
$ErrorActionPreference = "Continue"
& C:\mozilla-build\msys2\usr\bin\bash.exe -lc "bash /c/rfx/retrofox/scripts/make-phone-libs.sh" 2>&1 |
    ForEach-Object { "$_" } | Where-Object { $_ -notmatch 'stty:' }
$ErrorActionPreference = $rfxPrevEAP
if ($LASTEXITCODE -ne 0) { throw "make-phone-libs.sh failed" }

$LinkFlags = @(
    "/machine:arm", "/subsystem:console",
    "/libpath:$Sdk\vc143\lib\arm",
    "/libpath:$Sdk\ucrt\Lib\$UcrtVer\ucrt\arm",
    "/libpath:$Sdk\phonekit\lib\ARM",
    "/libpath:$PhoneLib",
    "libcmt.lib", "libvcruntime.lib", "libucrt.lib", "WindowsPhoneCore.lib", "kernel32.lib", "retrofox-compat.lib"
)

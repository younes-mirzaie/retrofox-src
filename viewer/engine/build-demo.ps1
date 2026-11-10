# Builds rfxview-demo.exe (stand-in engine for testing the app) for the phone.
#   .\viewer\engine\build-demo.ps1        build, copy to the phone, start it there
#   .\viewer\engine\build-demo.ps1 -NoRun build only
# On the phone it runs from C:\Data\Users\Public\Documents\retrofox and listens on port 8081.
param([switch]$NoRun)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\..\..\scripts\env.ps1"

$out = "$BuildRoot\tools\rfxview"
New-Item -ItemType Directory -Force $out | Out-Null
$objs = @()
foreach ($s in @("$PSScriptRoot\rfxview.cpp", "$PSScriptRoot\rfxview-demo.cpp")) {
    $o = Join-Path $out ([IO.Path]::GetFileNameWithoutExtension($s) + ".obj")
    & "$Llvm\clang-cl.exe" @CFlags /std:c++14 /EHsc /c $s "/Fo$o" 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { throw "Compile failed: $s" }
    $objs += $o
}
& "$Llvm\lld-link.exe" @LinkFlags "ws2_32.lib" @objs "/out:$out\rfxview-demo.exe" 2>&1 | ForEach-Object { "$_" }
if ($LASTEXITCODE -ne 0) { throw "Link failed." }
Write-Host "Built $out\rfxview-demo.exe" -ForegroundColor Cyan

if (-not $NoRun) {
    & "$Root\phone\phone-push.ps1" "$out\rfxview-demo.exe"
    # "start" so the telnet session returns while the demo keeps running
    & "$Root\phone\phone-cmd.ps1" "start C:\Data\Users\Public\Documents\retrofox\rfxview-demo.exe"
}

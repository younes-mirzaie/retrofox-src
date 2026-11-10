# Builds the Retrofox app and installs it on the USB-connected Lumia (or the emulator).
#   .\viewer\app\build-deploy.ps1             phone (ARM)
#   .\viewer\app\build-deploy.ps1 -Emulator   WP 8.1 emulator (x86)
# Needs VS 2015 with the Windows Phone 8.1 tools (same as HelloLumia).
param([string]$Configuration = "Release", [switch]$Emulator)
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
$pf86 = ${env:ProgramFiles(x86)}
$platform = if ($Emulator) { "x86" } else { "ARM" }
$target = if ($Emulator) { "xd" } else { "de" }

$msbuild = "$pf86\MSBuild\14.0\Bin\MSBuild.exe"
if (-not (Test-Path $msbuild)) { throw "MSBuild 14 not found. Install VS 2015 first." }
$deploy = "$pf86\Microsoft SDKs\Windows Phone\v8.1\Tools\AppDeploy\AppDeployCmd.exe"
if (-not (Test-Path $deploy)) {
    $found = Get-ChildItem "$pf86\Microsoft SDKs\Windows Phone" -Recurse -Filter AppDeployCmd.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { $deploy = $found.FullName } else { throw "AppDeployCmd.exe not found." }
}

Write-Host "Building ($platform)..." -ForegroundColor Cyan
& $msbuild .\Retrofox.sln /p:Configuration=$Configuration /p:Platform=$platform /p:VisualStudioVersion=14.0 /m /v:minimal
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

$appx = Get-ChildItem .\Retrofox -Recurse -Filter *.appx | Where-Object { $_.FullName -match "_$platform" } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $appx) { throw "No .appx produced. Check the build output." }
Write-Host "Package: $($appx.FullName)" -ForegroundColor Cyan
& $deploy /installlaunch $appx.FullName /targetdevice:$target

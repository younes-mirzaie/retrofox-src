# Builds a single-file native ARM tool for the phone and, with -Run, runs it there.
#   .\build-smoke.ps1                                    -> scripts\smoke\hello.c
#   .\build-smoke.ps1 -Source phone\probe\probe.c -Run -ToolArgs "CreatePipe","timeGetTime"
param(
    [string]$Source = "scripts\smoke\hello.c",
    [switch]$Run,
    [string[]]$ToolArgs = @()
)
. "$PSScriptRoot\env.ps1"

$src = Join-Path $Root $Source
$name = [IO.Path]::GetFileNameWithoutExtension($src)
$out = "$BuildRoot\tools"
New-Item -ItemType Directory -Force $out | Out-Null

& "$Llvm\clang-cl.exe" @CFlags /c $src "/Fo$out\$name.obj" 2>&1 | ForEach-Object { "$_" }
if ($LASTEXITCODE -ne 0) { throw "Compile failed." }
& "$Llvm\lld-link.exe" @LinkFlags "$out\$name.obj" "/out:$out\$name.exe" 2>&1 | ForEach-Object { "$_" }
if ($LASTEXITCODE -ne 0) { throw "Link failed." }
Write-Host "Built $out\$name.exe" -ForegroundColor Cyan

if ($Run) {
    & "$Root\phone\phone-push.ps1" "$out\$name.exe"
    & "$Root\phone\phone-cmd.ps1" ("C:\Data\Users\Public\Documents\retrofox\$name.exe " + ($ToolArgs -join ' '))
}

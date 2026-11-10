# Builds Retrofox for PC (RetrofoxPC.exe, WPF, .NET Framework 4 that ships with Windows) and
# packages it with the desktop engine:
#   .\build.ps1                    -> C:\rfx\retrofox-pc\RetrofoxPC.exe + engine\ (the desktop build's dist\bin)
#   .\build.ps1 -Zip <file.zip>    also zips that folder
param([string]$Out = "C:\rfx\retrofox-pc", [string]$Engine = "C:\rfx\obj-desktop\dist\bin", [string]$Zip = "")
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$fw = "$env:WINDIR\Microsoft.NET\Framework\v4.0.30319"
$wpf = "$fw\WPF"
New-Item -ItemType Directory -Force $Out | Out-Null
& "$fw\csc.exe" /nologo /target:winexe /optimize+ /out:"$Out\RetrofoxPC.exe" `
    /r:"$wpf\PresentationFramework.dll" /r:"$wpf\PresentationCore.dll" /r:"$wpf\WindowsBase.dll" `
    /r:"$fw\System.Xaml.dll" /r:"$fw\System.Web.Extensions.dll" "$here\RetrofoxPC.cs"
if ($LASTEXITCODE -ne 0) { throw "compile failed" }
# The engine: a copy of the desktop build, without the test tools
if (Test-Path "$Out\engine") { Remove-Item -Recurse -Force "$Out\engine" }
Copy-Item -Recurse $Engine "$Out\engine"
foreach ($f in "xpcshell.exe", "certutil.exe", "pk12util.exe", "xulrunner.exe", "xulrunner-stub.exe", "install_app.py", ".gdbinit", "README.xulrunner") {
    if (Test-Path "$Out\engine\$f") { Remove-Item -Force "$Out\engine\$f" }
}
Write-Host "Retrofox for PC: $Out\RetrofoxPC.exe" -ForegroundColor Green
if ($Zip) {
    if (Test-Path $Zip) { Remove-Item -Force $Zip }
    Compress-Archive -Path "$Out\*" -DestinationPath $Zip
    Write-Host "Zip: $Zip ($([math]::Round((Get-Item $Zip).Length / 1MB)) MB)" -ForegroundColor Green
}

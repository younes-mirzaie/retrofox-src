# Copies files from the phone's C:\Data\Users\Public\Documents\retrofox to a PC folder over MTP.
# Pair with phone-cmd.ps1 to stage system files there first (copy C:\windows\system32\x.dll ...).
# Usage: .\phone-pull.ps1 -Dest build\phone-dlls KERNELBASE.DLL KERNEL32LEGACY.DLL
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Mandatory = $true)][string]$Dest,
    [Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)][string[]]$Names
)
$ErrorActionPreference = "Stop"

$shell = New-Object -ComObject Shell.Application
function Get-Child($folder, $name) {
    $base = [IO.Path]::GetFileNameWithoutExtension($name)
    $folder.Items() | Where-Object { $_.Name -eq $name -or ($_.Name -eq $base -and -not $_.IsFolder) } | Select-Object -First 1
}
$phone = $shell.NameSpace(17).Items() | Where-Object { $_.Name -match 'Lumia|Windows Phone' } | Select-Object -First 1
if (-not $phone) { throw "Phone not found in This PC. Plug it in over USB and unlock the screen." }
$src = (Get-Child (Get-Child (Get-Child $phone.GetFolder 'Phone').GetFolder 'Documents').GetFolder 'retrofox').GetFolder

New-Item -ItemType Directory -Force $Dest | Out-Null
$destFull = (Resolve-Path $Dest).Path
foreach ($n in $Names) {
    $item = Get-Child $src $n
    if (-not $item) { throw "$n not found in Documents\retrofox on the phone." }
    $shell.NameSpace($destFull).CopyHere($item, 0x10)
    $deadline = (Get-Date).AddSeconds(120)
    while (-not (Test-Path (Join-Path $destFull $n))) {
        if ((Get-Date) -gt $deadline) { throw "Timed out pulling $n" }
        Start-Sleep -Milliseconds 500
    }
    Write-Host "Pulled $n -> $destFull\$n"
}

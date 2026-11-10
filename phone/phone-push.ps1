# Copies files to the USB-connected Lumia over MTP, into Phone\Documents\retrofox.
# On the phone that folder is C:\Data\Users\Public\Documents\retrofox.
# Usage: .\phone-push.ps1 build\js.exe build\test.js
#
# MTP's view of the phone is cached and reports an existing file name as "done" before an
# overwrite lands. So each file goes up under a unique temporary name, its size is checked
# on the phone over telnet, and only then is it renamed into place.
param([Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)][string[]]$Files)
$ErrorActionPreference = "Stop"

$shell = New-Object -ComObject Shell.Application
$phoneDir = "C:\Data\Users\Public\Documents\retrofox"

# Explorer may hide file extensions in MTP views, so match with or without one
function Get-Child($folder, $name) {
    $base = [IO.Path]::GetFileNameWithoutExtension($name)
    $folder.Items() | Where-Object { $_.Name -eq $name -or ($_.Name -eq $base -and -not $_.IsFolder) } | Select-Object -First 1
}

# Walks This PC > phone > Phone > Documents fresh each time; MTP item lists are cached
function Get-Docs {
    $phone = $shell.NameSpace(17).Items() | Where-Object { $_.Name -match 'Lumia|Windows Phone' } | Select-Object -First 1
    if (-not $phone) { throw "Phone not found in This PC. Plug it in over USB and unlock the screen." }
    (Get-Child (Get-Child $phone.GetFolder 'Phone').GetFolder 'Documents').GetFolder
}

function Wait-For($what, $test, $seconds = 120) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while (-not (& $test)) {
        if ((Get-Date) -gt $deadline) { throw "Timed out waiting for $what" }
        Start-Sleep -Milliseconds 500
    }
}

# File size on the phone (via telnet), or -1 if it isn't there
function Get-PhoneSize($name) {
    $out = & "$PSScriptRoot\phone-cmd.ps1" -IdleMs 1500 "for %I in (`"$phoneDir\$name`") do @echo SIZE=%~zI"
    $m = [regex]::Match(($out -join "`n"), 'SIZE=(\d+)')
    if ($m.Success) { [long]$m.Groups[1].Value } else { -1 }
}

if (-not (Get-Child (Get-Docs) 'retrofox')) {
    (Get-Docs).NewFolder('retrofox')
    Wait-For 'folder retrofox' { Get-Child (Get-Docs) 'retrofox' }
}

$stage = Join-Path $env:TEMP "rfxpush"
New-Item -ItemType Directory -Force $stage | Out-Null

foreach ($f in $Files) {
    $full = (Resolve-Path $f).Path
    $name = Split-Path $full -Leaf
    $size = (Get-Item $full).Length
    $tmp = "$name.$([DateTime]::Now.Ticks).part"
    Copy-Item $full (Join-Path $stage $tmp)

    (Get-Child (Get-Docs) 'retrofox').GetFolder.CopyHere((Join-Path $stage $tmp), 0x10)
    Wait-For "$name on the phone" { (Get-PhoneSize $tmp) -eq $size }
    & "$PSScriptRoot\phone-cmd.ps1" -IdleMs 1500 "move /y `"$phoneDir\$tmp`" `"$phoneDir\$name`"" | Out-Null
    if ((Get-PhoneSize $name) -ne $size) { throw "Copy of $name didn't land on the phone" }

    Remove-Item (Join-Path $stage $tmp)
    Write-Host "Copied $name -> $phoneDir\$name ($size bytes)"
}

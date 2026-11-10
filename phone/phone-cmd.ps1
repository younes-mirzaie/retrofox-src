# Runs commands on the Lumia through CMD Injector's telnet server and prints the output.
# Usage: .\phone-cmd.ps1 "ver" "dir C:\Data\Users\Public\Documents"
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)]
    [string[]]$Commands,
    [string]$PhoneIp = $(if ($env:RETROFOX_PHONE_IP) { $env:RETROFOX_PHONE_IP } else { throw "Set RETROFOX_PHONE_IP to the phone's Wi-Fi address" }),
    [int]$Port = 23,
    [int]$IdleMs = 2500
)
$ErrorActionPreference = "Stop"

$client = New-Object System.Net.Sockets.TcpClient
$connect = $client.BeginConnect($PhoneIp, $Port, $null, $null)
if (-not $connect.AsyncWaitHandle.WaitOne(5000)) { throw "Could not reach $PhoneIp`:$Port. Is CMD Injector's telnet running and the phone on Wi-Fi?" }
$client.EndConnect($connect)
$stream = $client.GetStream()

# Reads until the phone has been quiet for $IdleMs, dropping telnet negotiation (IAC) bytes
function Read-Phone {
    $buf = New-Object byte[] 65536
    $text = New-Object System.Text.StringBuilder
    $quiet = [Diagnostics.Stopwatch]::StartNew()
    while ($quiet.ElapsedMilliseconds -lt $IdleMs) {
        if ($stream.DataAvailable) {
            $n = $stream.Read($buf, 0, $buf.Length)
            for ($i = 0; $i -lt $n; $i++) {
                if ($buf[$i] -eq 255) { $i += 2; continue }
                [void]$text.Append([char]$buf[$i])
            }
            $quiet.Restart()
        } else { Start-Sleep -Milliseconds 100 }
    }
    $text.ToString()
}

Read-Phone | Out-Null  # banner
foreach ($cmd in $Commands) {
    # The phone's terminal redraws (and wraps) what we type, so bracket the real output with markers
    $bytes = [Text.Encoding]::ASCII.GetBytes("echo ##B## & $cmd & echo ##E##`r`n")
    $stream.Write($bytes, 0, $bytes.Length)
    # Cursor moves to column 1 become newlines; other ANSI escapes are dropped
    $out = (Read-Phone) -replace "\x1b\[\d+;1H", "`n" -replace "\x1b\[[0-9;?]*[A-Za-z]", ""
    $lines = $out -split "`r?`n"
    # Last occurrences: a wrapped echo of the typed command can leave a stray marker line earlier
    $begin = [Array]::FindLastIndex($lines, [Predicate[string]] { param($l) $l.Trim() -eq '##B##' })
    $end = [Array]::FindLastIndex($lines, [Predicate[string]] { param($l) $l.Trim() -eq '##E##' })
    ">$cmd"
    if ($begin -ge 0 -and $end -gt $begin + 1) { ($lines[($begin + 1)..($end - 1)]) -join "`n" }
    elseif ($begin -lt 0 -or $end -lt $begin) { $out }
}
$client.Close()

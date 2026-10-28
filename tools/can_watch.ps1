# Знаходить Master у локальній мережі, перевіряє снайфер і запускає приймач CAN-кадрів.
#
#   powershell -ExecutionPolicy Bypass -File tools\can_watch.ps1
#   powershell -ExecutionPolicy Bypass -File tools\can_watch.ps1 -Id 0x7E0
#   powershell -ExecutionPolicy Bypass -File tools\can_watch.ps1 -Check     # тільки перевірка
param(
    [string]$Id = "",
    [switch]$Check
)

$ErrorActionPreference = 'SilentlyContinue'
$root = Split-Path -Parent $PSScriptRoot
$known = @('192.168.88.244')

function Test-Player([string]$ip) {
    try {
        $r = Invoke-WebRequest -Uri "http://$ip/version" -TimeoutSec 2 -UseBasicParsing
        return ($r.StatusCode -eq 200 -and $r.Content -match 'player_build')
    } catch { return $false }
}

Write-Host "Shukayu Master u merezhi..." -ForegroundColor Cyan
$ip = $null
foreach ($cand in $known) { if (Test-Player $cand) { $ip = $cand; break } }

if (-not $ip) {
    $subnet = '192.168.88.'
    $tasks = @()
    for ($i = 1; $i -le 254; $i++) {
        $p = New-Object System.Net.NetworkInformation.Ping
        $tasks += [pscustomobject]@{ IP = "$subnet$i"; Task = $p.SendPingAsync("$subnet$i", 400) }
    }
    [System.Threading.Tasks.Task]::WaitAll($tasks.Task) | Out-Null
    foreach ($t in $tasks) {
        if ($t.Task.Result.Status -eq 'Success' -and (Test-Player $t.IP)) { $ip = $t.IP; break }
    }
}

if (-not $ip) {
    Write-Host "Master ne znaydeno. Perevin, shcho plata v merezhi (STA) abo vkazhy IP." -ForegroundColor Red
    exit 1
}

Write-Host "Master: $ip" -ForegroundColor Green
try {
    $st = (Invoke-WebRequest -Uri "http://$ip/player_status" -TimeoutSec 3 -UseBasicParsing).Content
    Write-Host "status: $st"
} catch { Write-Host "status: n/a" }

if ($Check) { exit 0 }

$argsList = @("$root\tools\can_monitor.py")
if ($Id) { $argsList += @('--id', $Id) }

Write-Host "Listening UDP 43605 (Ctrl+C to stop)..." -ForegroundColor Cyan
& python @argsList

# Швидкий скан локальної мережі: шукаємо ESP32 (Ford Player) за відповіддю /version
$ErrorActionPreference = 'SilentlyContinue'
$subnet = '192.168.88.'
$tasks = @()
for ($i = 1; $i -le 254; $i++) {
    $ip = "$subnet$i"
    $p = New-Object System.Net.NetworkInformation.Ping
    $tasks += [pscustomobject]@{ IP = $ip; Task = $p.SendPingAsync($ip, 700) }
}
[System.Threading.Tasks.Task]::WaitAll($tasks.Task) | Out-Null
$alive = $tasks | Where-Object { $_.Task.Result.Status -eq 'Success' } | Select-Object -ExpandProperty IP
"alive hosts: $($alive.Count)"
$alive | ForEach-Object { $_ }

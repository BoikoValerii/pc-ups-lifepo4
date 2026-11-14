# Выключение ПК по сигналу монитора ИБП (ESP32, http://pc-ups.local/data).
# Каждые 15 с читает JSON; при "shutdown": true — выключение через 2 минуты,
# если за это время свет вернулся (shutdown снова false) — отмена.
# ESP не отвечает — ничего не делает (ПК не выключается «вслепую»).
param(
  [string]$Esp = 'pc-ups.local',
  [int]$Interval = 15
)

$log = Join-Path $PSScriptRoot 'pc-shutdown.log'
function Log($m) { "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')  $m" | Add-Content -Path $log -Encoding utf8 }

# mDNS-имя в Windows резолвится медленно — запоминаем IP и обновляем его при ошибках
$addr = $null
$pending = $false
Log "старт, монитор $Esp"

while ($true) {
  try {
    if (-not $addr) {
      $addr = (Resolve-DnsName $Esp -Type A -ErrorAction Stop | Select-Object -First 1).IPAddress
      if (-not $addr) { $addr = $Esp }
      Log "монитор: $addr"
    }
    $d = Invoke-RestMethod "http://$addr/data" -TimeoutSec 5
    if ($d.shutdown -and -not $pending) {
      $pending = $true
      Log "сигнал выключения: заряд $($d.soc)%, $($d.v) В"
      shutdown.exe /s /t 120 /c "ИБП: батарея $($d.soc)%. ПК выключится через 2 минуты - сохраните работу"
    } elseif (-not $d.shutdown -and $pending) {
      $pending = $false
      Log "отмена: свет вернулся"
      shutdown.exe /a
    }
  } catch {
    $addr = $null
  }
  Start-Sleep -Seconds $Interval
}

# Ставит pc-shutdown.ps1 в планировщик: запуск при старте Windows от SYSTEM, скрыто.
# Запускать PowerShell от имени администратора.
$script = Join-Path $PSScriptRoot 'pc-shutdown.ps1'
$action = New-ScheduledTaskAction -Execute 'powershell.exe' `
  -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$script`""
$trigger = New-ScheduledTaskTrigger -AtStartup
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
  -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1)
Register-ScheduledTask -TaskName 'PC UPS shutdown' -Action $action -Trigger $trigger -Settings $settings `
  -User 'SYSTEM' -RunLevel Highest -Force | Out-Null
Start-ScheduledTask -TaskName 'PC UPS shutdown'
"Готово: задача 'PC UPS shutdown' запущена, журнал — pc-shutdown.log рядом со скриптом"

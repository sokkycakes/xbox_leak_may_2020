<#
.SYNOPSIS
  One-time setup inside a Crimson partition VM. Run as administrator in the VM.

  - signs the given account in automatically, so the agent starts on boot
  - turns off sleep, the lock screen and the screen saver
  - System OS: solid magenta desktop, which the host treats as see-through,
    so only the shell's own windows appear over the game
  - Game OS: plain black desktop

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File C:\Crimson\Setup-Guest.ps1 -User crimson -Password ********
#>
param(
    [Parameter(Mandatory)] [string] $User,
    [Parameter(Mandatory)] [string] $Password
)
$ErrorActionPreference = 'Stop'
$role = (Get-Content C:\Crimson\role.txt -ErrorAction SilentlyContinue | Select-Object -First 1)
if (-not $role) { $role = 'game' }

# Auto sign-in. The password is stored in the registry in plain text, which is
# acceptable for a throwaway prototype partition and nothing else.
$wl = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon'
Set-ItemProperty $wl AutoAdminLogon '1'
Set-ItemProperty $wl DefaultUserName $User
Set-ItemProperty $wl DefaultPassword $Password
Set-ItemProperty $wl DefaultDomainName $env:COMPUTERNAME

# Never sleep, never lock.
powercfg /change standby-timeout-ac 0
powercfg /change monitor-timeout-ac 0
New-Item 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\Personalization' -Force | Out-Null
Set-ItemProperty 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\Personalization' NoLockScreen 1 -Type DWord
Set-ItemProperty 'HKCU:\Control Panel\Desktop' ScreenSaveActive '0'

# Desktop colour (applies at next sign-in).
$colour = if ($role -eq 'system') { '255 0 255' } else { '0 0 0' }
Set-ItemProperty 'HKCU:\Control Panel\Colors' Background $colour
Set-ItemProperty 'HKCU:\Control Panel\Desktop' WallPaper ''
# hide desktop icons and auto-hide the taskbar so they don't show in the stream
Set-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' HideIcons 1 -Type DWord
$sr = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\StuckRects3'
if (Test-Path $sr) { $v = (Get-ItemProperty $sr).Settings; $v[8] = 3; Set-ItemProperty $sr Settings $v }

# Name the partition so it's easy to tell apart.
$name = if ($role -eq 'system') { 'CRIMSON-SYSTEM' } else { 'CRIMSON-GAME' }
if ($env:COMPUTERNAME -ne $name) { Rename-Computer -NewName $name -Force }

Write-Host "Set up as the $role partition. Restart the VM to apply."

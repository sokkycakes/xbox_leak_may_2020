# Undoes fse\Install-FSE.ps1: removes the "Blades Dashboard" app package registration (the files in
# this folder stay) and puts back the settings saved in fse\settings-before-install.txt.
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Backup = Join-Path $Root 'fse\settings-before-install.txt'
$GameBarKey = 'HKCU:\Software\Microsoft\GameBar'
$GamingKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\GamingConfiguration'

function Restore($key, $name, $value, $type) {
    if ($value -eq '<none>') { Remove-ItemProperty -Path $key -Name $name -ErrorAction SilentlyContinue }
    else { Set-ItemProperty -Path $key -Name $name -Value $value -Type $type }
}
try {
    Get-Process -Name 'Blades-Dashboard' -ErrorAction SilentlyContinue | Stop-Process -Force
    $pkg = Get-AppxPackage -Name 'Crimson.BladesDashboard' -ErrorAction SilentlyContinue
    if ($pkg) { Remove-AppxPackage -Package $pkg.PackageFullName; Write-Host 'App package removed.' -ForegroundColor Green }
    $saved = @{}
    if (Test-Path $Backup) { foreach ($line in Get-Content $Backup) { $i = $line.IndexOf('='); if ($i -gt 0) { $saved[$line.Substring(0, $i)] = $line.Substring($i + 1) } } }
    foreach ($name in 'LegacyGamepadXboxGuideButtonEnabled', 'UseNexusForGameBarEnabled') {
        if ($saved.ContainsKey($name)) { Restore $GameBarKey $name $saved[$name] DWord }
    }
    Write-Host 'Game Bar on the Xbox button restored.' -ForegroundColor Green
    if ($saved.ContainsKey('GamingHomeApp')) { Restore $GamingKey 'GamingHomeApp' $saved['GamingHomeApp'] String; Write-Host 'Full Screen Experience home app restored.' -ForegroundColor Green }
    $elevated = @()
    if ($saved.ContainsKey('DeviceForm')) {
        if ($saved['DeviceForm'] -eq '<none>') { $elevated += 'reg delete "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OEM" /v DeviceForm /f' }
        else { $elevated += ('reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OEM" /v DeviceForm /t REG_DWORD /d ' + $saved['DeviceForm'] + ' /f') }
    }
    if ($saved.ContainsKey('DeveloperMode')) {
        $answer = Read-Host 'Developer Mode was turned on by the installer. Turn it off again? (Y/N)'
        if ($answer -match '^[Yy]') {
            if ($saved['DeveloperMode'] -eq '<none>') { $elevated += 'reg delete "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock" /v AllowDevelopmentWithoutDevLicense /f' }
            else { $elevated += ('reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock" /v AllowDevelopmentWithoutDevLicense /t REG_DWORD /d ' + $saved['DeveloperMode'] + ' /f') }
        }
    }
    if ($elevated.Count) {
        Write-Host 'Windows will ask for permission to restore the system settings.' -ForegroundColor Yellow
        try { Start-Process -FilePath 'cmd.exe' -ArgumentList ('/c ' + ($elevated -join ' & ')) -Verb RunAs -Wait -WindowStyle Hidden }
        catch { Write-Host 'System settings were not restored (permission was not given). Run this again to retry.' -ForegroundColor Yellow; exit 1 }
    }
    if (Test-Path $Backup) { Remove-Item $Backup }
    Write-Host 'Done.' -ForegroundColor Green
} catch {
    Write-Host ('Uninstall failed: ' + $_.Exception.Message) -ForegroundColor Red
    exit 1
}

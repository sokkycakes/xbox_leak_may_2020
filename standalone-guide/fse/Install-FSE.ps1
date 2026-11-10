# Registers this standalone-guide folder as the Windows 11 Full Screen Experience (Xbox mode)
# Home app "Blades Dashboard" and gives the Xbox button to the Blades Guide instead of Game Bar.
# Run through "Install FSE Home.cmd". Every change is undone by "Uninstall FSE Home.cmd".
# Method from the MIT projects SteamFSE (ND091024/SteamFSE) and AnyFSE (ashpynov/AnyFSE).
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$PackageName = 'Crimson.BladesDashboard'
$Backup = Join-Path $Root 'fse\settings-before-install.txt'
$Log = Join-Path $Root 'fse\install.log'
$GameBarKey = 'HKCU:\Software\Microsoft\GameBar'
$GamingKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\GamingConfiguration'
$OemKey = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OEM'

function Say($text, $color = 'Gray') { Write-Host $text -ForegroundColor $color; Add-Content -Path $Log -Value ("[{0}] {1}" -f (Get-Date -Format s), $text) }
function Get-Value($key, $name) { try { (Get-ItemProperty -Path $key -Name $name -ErrorAction Stop).$name } catch { $null } }
function Save-Backup($name, $value) {
    # First install only: later installs keep the settings from before the very first one.
    $lines = @(); if (Test-Path $Backup) { $lines = Get-Content $Backup }
    if ($lines | Where-Object { $_ -like "$name=*" }) { return }
    if ($null -eq $value) { $value = '<none>' }
    Add-Content -Path $Backup -Value "$name=$value"
}
function Run-Elevated($commands) {
    # One UAC prompt for HKLM changes; returns $true when the elevated step finished.
    try {
        $p = Start-Process -FilePath 'cmd.exe' -ArgumentList ('/c ' + ($commands -join ' & ')) -Verb RunAs -Wait -PassThru -WindowStyle Hidden
        return ($p.ExitCode -eq 0)
    } catch { return $false }   # the permission prompt was declined
}

try {
    Say 'Blades Dashboard: Full Screen Experience home app install' 'Cyan'
    if (-not (Test-Path (Join-Path $Root 'build\Blades-Dashboard.exe'))) { throw "build\Blades-Dashboard.exe is missing in $Root" }
    if (-not (Test-Path (Join-Path $Root 'AppxManifest.xml'))) { throw "AppxManifest.xml is missing in $Root" }

    $build = [int](Get-CimInstance Win32_OperatingSystem).BuildNumber
    Say "Windows build $build"
    if ($build -lt 26100) { Say 'Windows 11 24H2 (build 26100) or newer is needed for the Full Screen Experience.' 'Yellow' }

    # 1. Developer Mode: needed to register an unsigned package with the gamingHome capability.
    $dev = Get-Value 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock' 'AllowDevelopmentWithoutDevLicense'
    if ($dev -ne 1) {
        Say 'Developer Mode is off. Windows will ask for permission to turn it on.' 'Yellow'
        Save-Backup 'DeveloperMode' $dev
        $ok = Run-Elevated @('reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock" /v AllowDevelopmentWithoutDevLicense /t REG_DWORD /d 1 /f')
        $dev = Get-Value 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock' 'AllowDevelopmentWithoutDevLicense'
        if ($dev -ne 1) {
            Start-Process 'ms-settings:developers'
            throw 'Developer Mode is still off. Turn on "Developer Mode" in the Settings page that just opened, then run the installer again.'
        }
        Say 'Developer Mode turned on.' 'Green'
    }

    # 2. The app package, registered in place from this folder.
    $old = Get-AppxPackage -Name $PackageName -ErrorAction SilentlyContinue
    if ($old) { Say "Replacing the earlier registration $($old.PackageFullName)"; Remove-AppxPackage -Package $old.PackageFullName; Start-Sleep -Milliseconds 700 }
    Get-Process -Name 'Blades-Dashboard' -ErrorAction SilentlyContinue | Stop-Process -Force
    Add-AppxPackage -Register (Join-Path $Root 'AppxManifest.xml') -ForceApplicationShutdown
    $pkg = Get-AppxPackage -Name $PackageName
    if (-not $pkg) { throw 'The package did not register.' }
    $aumid = $pkg.PackageFamilyName + '!App'
    Say "Registered $($pkg.PackageFullName)" 'Green'

    # 3. Xbox button -> our Guide, not Game Bar (Xbox mode and desktop).
    if (-not (Test-Path $GameBarKey)) { New-Item -Path $GameBarKey -Force | Out-Null }
    foreach ($name in 'LegacyGamepadXboxGuideButtonEnabled', 'UseNexusForGameBarEnabled') {
        Save-Backup $name (Get-Value $GameBarKey $name)
        Set-ItemProperty -Path $GameBarKey -Name $name -Value 0 -Type DWord
    }
    Say 'Game Bar no longer opens on the Xbox button (Win+G still opens it).' 'Green'

    # 4. Home app for the Full Screen Experience.
    if (-not (Test-Path $GamingKey)) { New-Item -Path $GamingKey -Force | Out-Null }
    Save-Backup 'GamingHomeApp' (Get-Value $GamingKey 'GamingHomeApp')
    Set-ItemProperty -Path $GamingKey -Name 'GamingHomeApp' -Value $aumid -Type String
    Say "Full Screen Experience home app set to Blades Dashboard ($aumid)." 'Green'

    # 5. Windows only offers the Full Screen Experience on handhelds (OEM DeviceForm 46) unless a
    #    newer build opens it to every PC. Offered, not forced.
    $form = Get-Value $OemKey 'DeviceForm'
    if ($form -ne 46) {
        Write-Host ''
        Write-Host 'This PC is not marked as a gaming handheld, so Windows may hide the Full Screen Experience.' -ForegroundColor Yellow
        $answer = Read-Host 'Mark it as a handheld so the Full Screen Experience is offered? (Y/N)'
        if ($answer -match '^[Yy]') {
            Save-Backup 'DeviceForm' $form
            if (Run-Elevated @('reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OEM" /v DeviceForm /t REG_DWORD /d 46 /f')) {
                Say 'Marked as a handheld (sign out and back in, or restart, for Windows to notice).' 'Green'
            } else { Say 'Not changed (permission was not given).' 'Yellow' }
        } else { Say 'Left as it is.' }
    }

    Write-Host ''
    Write-Host 'Done.' -ForegroundColor Green
    Write-Host 'Open the Full Screen Experience (Xbox mode). Blades Dashboard is its Home now.'
    Write-Host 'If it still opens the Xbox app, pick Home app > Blades Dashboard in Settings > Gaming > Full screen experience.'
    Write-Host 'Xbox button: Guide over any game. B closes it, Y goes to the Blades dashboard.'
    Write-Host "Log: $Log"
} catch {
    Say ("Install failed: " + $_.Exception.Message) 'Red'
    exit 1
}

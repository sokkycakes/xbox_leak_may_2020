<#
.SYNOPSIS
  Builds EraOS-style minimal images for the Crimson Game OS and System OS,
  based on Windows PE, and optionally creates the two VMs that boot them.

.DESCRIPTION
  EraOS (the Xbox One game OS) is a cut-down Windows kernel with no shell,
  mounted read-only, with the game bringing the runtime pieces it needs.
  Windows PE is the closest thing Microsoft ships publicly:

    - one WIM, a few hundred MB, booted into a RAM disk (X:) and never
      written back: a reboot always starts clean, like a read-only XVD
    - no Explorer, no Store, no services beyond the basics
    - the same NT kernel, Win32 and D3D runtime family as desktop Windows

  This script:
    1. copies a fresh WinPE from the Windows ADK (copype)
    2. adds only the optional components the role needs
    3. "donates" the missing runtime files from a full Windows image
       (install.wim on a Windows ISO): D3D/DXGI, audio, XAudio2, Hyper-V
       socket driver, VC++ runtime. List: era-files.txt
    4. adds the host GPU driver for GPU partitioning (HostDriverStore)
    5. adds crimson-agent and a winpeshl.ini so the agent, not cmd.exe, is
       the first program (the System OS also gets its shell)
    6. strips what neither role needs (setup, WinRE tools, fonts, locales)
    7. writes crimson-game.iso and crimson-system.iso
    8. with -CreateVms: two Gen 2 VMs that boot the ISOs read-only, plus a
       small data disk each for saves and settings, and for the Game OS an
       optional game disk attached like the console's game package.

  Needs, on the build PC (elevated PowerShell):
    - Windows ADK and the WinPE add-on (same version as the donor ISO's build)
    - a Windows 10/11 ISO or install.wim of the same build, as the donor

.EXAMPLE
  .\New-CrimsonEraImage.ps1 -DonorIso D:\iso\Win11_24H2.iso -CreateVms
.EXAMPLE
  .\New-CrimsonEraImage.ps1 -DonorIso D:\iso\Win11_24H2.iso -SystemShell C:\Blades\Blades-Dashboard.exe -GameDisk D:\games\mygame.vhdx -CreateVms
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $DonorIso,
    [int]    $DonorIndex = 0,               # 0 = first Pro/Enterprise/IoT edition found
    [string] $WorkDir = "C:\CrimsonEra",
    [string] $SystemShell = "",             # exe (and its folder) used as the System OS shell
    [string] $GameDisk = "",                # VHDX holding the game, attached to the Game OS
    [switch] $NoGpuDriver,
    [switch] $KeepFonts,
    [switch] $CreateVms,
    [int]    $GameCores = 6, [int] $GameMemoryGB = 8,
    [int]    $SystemCores = 2, [int] $SystemMemoryGB = 2
)
$ErrorActionPreference = 'Stop'
$here      = $PSScriptRoot
$AgentExe  = Join-Path $here '..\..\build\crimson-agent.exe'
$ServiceId = 'c71b0e5a-3f2d-4e8b-9a61-7c0de0c41a57'

# ---------- ADK ----------
$adk = "${env:ProgramFiles(x86)}\Windows Kits\10\Assessment and Deployment Kit"
$peRoot = "$adk\Windows Preinstallation Environment"
$ocDir  = "$peRoot\amd64\WinPE_OCs"
$oscdimg = "$adk\Deployment Tools\amd64\Oscdimg\oscdimg.exe"
if (-not (Test-Path "$peRoot\amd64\en-us\winpe.wim")) {
    throw "Windows ADK + WinPE add-on not found. Install both from https://learn.microsoft.com/windows-hardware/get-started/adk-install"
}

function Invoke-Dism { & dism.exe /English @args; if ($LASTEXITCODE) { throw "dism $args failed ($LASTEXITCODE)" } }
function MB($path) { [math]::Round(((Get-ChildItem $path -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum) / 1MB) }

# ---------- donor ----------
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
$donorMount = Join-Path $WorkDir 'donor'
New-Item -ItemType Directory -Force -Path $donorMount | Out-Null
$iso = Mount-DiskImage -ImagePath $DonorIso -PassThru
$isoDrive = ($iso | Get-Volume).DriveLetter
$installWim = Get-ChildItem "${isoDrive}:\sources" -Filter 'install.*' | Where-Object { $_.Extension -in '.wim', '.esd' } | Select-Object -First 1
if ($installWim.Extension -eq '.esd') {
    # DISM can't mount an ESD; export the edition we need to a WIM first
    $wim = Join-Path $WorkDir 'install.wim'
    if ($DonorIndex -eq 0) { $DonorIndex = 1 }
    Invoke-Dism /Export-Image /SourceImageFile:$($installWim.FullName) /SourceIndex:$DonorIndex /DestinationImageFile:$wim /Compress:max
    $installWim = Get-Item $wim; $DonorIndex = 1
}
if ($DonorIndex -eq 0) {
    $DonorIndex = (Get-WindowsImage -ImagePath $installWim.FullName |
        Where-Object { $_.ImageName -match 'Pro|Enterprise|IoT' } | Select-Object -First 1).ImageIndex
}
Write-Host "Donor: $($installWim.FullName) index $DonorIndex"
Invoke-Dism /Mount-Image /ImageFile:$($installWim.FullName) /Index:$DonorIndex /MountDir:$donorMount /ReadOnly

# ---------- one image per role ----------
function Build-Role([string] $Role) {
    $pe = Join-Path $WorkDir "pe-$Role"
    if (Test-Path $pe) { Remove-Item $pe -Recurse -Force }
    & "$peRoot\copype.cmd" amd64 $pe | Out-Null
    $mount = Join-Path $pe 'mount'
    Invoke-Dism /Mount-Image /ImageFile:"$pe\media\sources\boot.wim" /Index:1 /MountDir:$mount

    # 2. optional components: only WMI (several drivers' installers and games query it)
    foreach ($oc in 'WinPE-WMI') {
        Invoke-Dism /Image:$mount /Add-Package /PackagePath:"$ocDir\$oc.cab"
        Invoke-Dism /Image:$mount /Add-Package /PackagePath:"$ocDir\en-us\${oc}_en-us.cab"
    }

    # 3. donated runtime files
    $list = Get-Content (Join-Path $here 'era-files.txt') | Where-Object { $_ -and -not $_.StartsWith('#') }
    foreach ($line in $list) {
        $roles, $rel = $line -split '\s+', 2
        if ($roles -ne 'both' -and $roles -ne $Role) { continue }
        foreach ($src in Get-ChildItem (Join-Path $donorMount $rel) -ErrorAction SilentlyContinue) {
            $dst = Join-Path $mount $src.FullName.Substring($donorMount.Length + 1)
            New-Item -ItemType Directory -Force -Path (Split-Path $dst) | Out-Null
            Copy-Item $src.FullName $dst -Force -Recurse
        }
    }
    # services and COM registrations for audio and hvsocket, merged from the donor's hives
    & (Join-Path $here 'Import-EraRegistry.ps1') -Donor $donorMount -Target $mount -Role $Role

    # 4. host GPU driver for GPU-P
    if (-not $NoGpuDriver) {
        $gpu = Get-CimInstance Win32_VideoController | Where-Object { $_.PNPDeviceID -like 'PCI*' } | Select-Object -First 1
        if ($gpu) {
            $files = Get-CimInstance Win32_PnPSignedDriverCIMDataFile | Where-Object { $_.Antecedent.DeviceID -eq $gpu.PNPDeviceID }
            $dirs = $files | ForEach-Object { $_.Dependent.Name } | Where-Object { $_ -like '*\DriverStore\FileRepository\*' } |
                ForEach-Object { ($_ -split '\\FileRepository\\')[1].Split('\')[0] } | Sort-Object -Unique
            foreach ($d in $dirs) {
                $dst = Join-Path $mount "Windows\System32\HostDriverStore\FileRepository\$d"
                New-Item -ItemType Directory -Force -Path $dst | Out-Null
                Copy-Item "$env:SystemRoot\System32\DriverStore\FileRepository\$d\*" $dst -Recurse -Force
            }
            foreach ($f in $files) {
                $p = $f.Dependent.Name
                if ($p -notlike '*\DriverStore\*' -and $p -like "$env:SystemRoot\System32\*" -and (Test-Path $p)) {
                    Copy-Item $p (Join-Path $mount $p.Substring(3)) -Force
                }
            }
            Write-Host "  GPU driver for $($gpu.Name) added"
        }
    }

    # 5. agent first, no command prompt
    $crimson = Join-Path $mount 'Crimson'
    New-Item -ItemType Directory -Force -Path $crimson | Out-Null
    Copy-Item $AgentExe $crimson
    if ($Role -eq 'game') {
        $agentArgs = '--role game --name "Game OS (era)" --hv --desktop --size 1920x1080 --fps 60'
        $launch = 'GAME'   # launch-game.cmd on whichever drive the game disk mounts as
    } else {
        $agentArgs = '--role system --name "System OS (era)" --hv --desktop --size 1920x1080 --fps 30 --guide-key F1'
        $launch = ''
        if ($SystemShell) {
            $shellDir = Join-Path $crimson 'shell'
            Copy-Item (Split-Path $SystemShell) $shellDir -Recurse
            $launch = "X:\Crimson\shell\$(Split-Path $SystemShell -Leaf)"
        }
    }
    @"
@echo off
wpeinit
wpeutil DisableFirewall
net start AudioEndpointBuilder >nul 2>&1
net start Audiosrv >nul 2>&1
rem saves/settings disk (D:) and, for the game OS, the game disk
for %%d in (C D E F G) do if exist %%d:\Crimson\data.tag set CRIMSON_DATA=%%d:
start "" /min X:\Crimson\crimson-agent.exe $agentArgs
$(if ($launch -eq 'GAME') { 'for %%d in (C D E F G H I) do if exist %%d:\Crimson\launch-game.cmd (set CRIMSON_GAME=%%d:& start "" /d %%d:\ cmd /c %%d:\Crimson\launch-game.cmd)' } elseif ($launch) { "if exist `"$launch`" start `"`" `"$launch`"" })
rem keep the session alive; nothing is shown, the host owns the screen
:idle
ping -n 3600 127.0.0.1 >nul
goto idle
"@ | Set-Content (Join-Path $crimson 'start.cmd') -Encoding ASCII
    "[LaunchApps]`r`n%SYSTEMDRIVE%\Crimson\start.cmd`r`n" |
        Set-Content (Join-Path $mount 'Windows\System32\winpeshl.ini') -Encoding ASCII

    # 6. strip what neither partition uses
    $strip = @(
        'Windows\System32\Recovery', 'Windows\System32\WindowsPowerShell', 'Windows\System32\oobe',
        'Windows\System32\setup*', 'Windows\System32\sysprep', 'Windows\System32\migration',
        'Windows\System32\wbem\Repository\*.bak', 'Windows\System32\spool',
        'sources'
    )
    if (-not $KeepFonts) {
        # keep Segoe UI + symbols + the console font; the rest are for locales we don't ship
        Get-ChildItem "$mount\Windows\Fonts" -File | Where-Object { $_.Name -notmatch '^(segoeui|seguisym|consola|cour|arial|marlett|vga|app|cga|ega|8514|ssee|sseri|serife|smalle)' } |
            Remove-Item -Force -ErrorAction SilentlyContinue
    }
    foreach ($s in $strip) { Remove-Item (Join-Path $mount $s) -Recurse -Force -ErrorAction SilentlyContinue }
    Get-ChildItem "$mount\Windows\System32" -Directory | Where-Object { $_.Name -match '^[a-z]{2}-[A-Z]{2}$' -and $_.Name -ne 'en-US' } |
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue

    $expanded = MB $mount
    Invoke-Dism /Image:$mount /Cleanup-Image /StartComponentCleanup /ResetBase
    Invoke-Dism /Unmount-Image /MountDir:$mount /Commit
    # recompress tightly
    $tmp = "$pe\boot-max.wim"
    Invoke-Dism /Export-Image /SourceImageFile:"$pe\media\sources\boot.wim" /SourceIndex:1 /DestinationImageFile:$tmp /Compress:max /Bootable
    Move-Item $tmp "$pe\media\sources\boot.wim" -Force
    $wimMB = [math]::Round((Get-Item "$pe\media\sources\boot.wim").Length / 1MB)

    # 7. ISO (UEFI only, no "press any key" prompt)
    $iso = Join-Path $WorkDir "crimson-$Role.iso"
    $efisys = "$pe\fwfiles\efisys_noprompt.bin"
    if (-not (Test-Path $efisys)) { $efisys = "$pe\fwfiles\efisys.bin" }
    & $oscdimg -m -o -u2 -udfver102 "-bootdata:1#pEF,e,b$efisys" "$pe\media" $iso | Out-Null
    Write-Host ("  crimson-$Role: {0} MB in RAM, {1} MB WIM, ISO {2}" -f $expanded, $wimMB, $iso)
    return $iso
}

try {
    $gameIso   = Build-Role 'game'
    $systemIso = Build-Role 'system'
} finally {
    Invoke-Dism /Unmount-Image /MountDir:$donorMount /Discard
    Dismount-DiskImage -ImagePath $DonorIso | Out-Null
}

# ---------- 8. VMs ----------
if ($CreateVms) {
    $key = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Virtualization\GuestCommunicationServices\$ServiceId"
    New-Item -Path $key -Force | Out-Null
    Set-ItemProperty -Path $key -Name ElementName -Value 'Crimson partition link'
    $vmRoot = Join-Path $WorkDir 'vms'
    $gpuOk = [bool](Get-VMHostPartitionableGpu -ErrorAction SilentlyContinue)

    function New-EraVm($Name, $Iso, $Cores, $MemGB, $Reserve, $Weight, $GpuShare, $Extra) {
        if (Get-VM $Name -ErrorAction SilentlyContinue) { Write-Host "$Name exists, skipped"; return }
        $vm = New-VM -Name $Name -Generation 2 -MemoryStartupBytes ($MemGB * 1GB) -NoVHD -Path $vmRoot
        Set-VM -VM $vm -StaticMemory -ProcessorCount $Cores -CheckpointType Disabled -AutomaticStopAction TurnOff
        Set-VMProcessor -VM $vm -Reserve $Reserve -RelativeWeight $Weight
        Set-VMVideo -VM $vm -ResolutionType Single -HorizontalResolution 1920 -VerticalResolution 1080
        $dvd = Add-VMDvdDrive -VM $vm -Path $Iso -Passthru
        # small writable disk for saves and settings, like the console's separate data storage
        $data = Join-Path $vmRoot "$Name-data.vhdx"
        if (-not (Test-Path $data)) { New-VHD -Path $data -SizeBytes 2GB -Dynamic | Out-Null }
        Add-VMHardDiskDrive -VM $vm -Path $data
        if ($Extra) { Add-VMHardDiskDrive -VM $vm -Path $Extra }
        Set-VMFirmware -VM $vm -FirstBootDevice $dvd -EnableSecureBoot On -SecureBootTemplate MicrosoftWindows
        if ($gpuOk) {
            Set-VM -VM $vm -GuestControlledCacheTypes $true -LowMemoryMappedIoSpace 1GB -HighMemoryMappedIoSpace 32GB
            Add-VMGpuPartitionAdapter -VM $vm
            $v = [uint64](1000000000 * $GpuShare / 100)
            Set-VMGpuPartitionAdapter -VM $vm -MinPartitionVRAM $v -MaxPartitionVRAM $v -OptimalPartitionVRAM $v `
                -MinPartitionCompute $v -MaxPartitionCompute $v -OptimalPartitionCompute $v `
                -MinPartitionEncode $v -MaxPartitionEncode $v -OptimalPartitionEncode $v `
                -MinPartitionDecode $v -MaxPartitionDecode $v -OptimalPartitionDecode $v
        }
        Write-Host "Created $Name"
    }
    New-EraVm 'Crimson-Era-Game'   $gameIso   $GameCores   $GameMemoryGB   100 1000 80 $GameDisk
    New-EraVm 'Crimson-Era-System' $systemIso $SystemCores $SystemMemoryGB 10  200  20 $null
    Write-Host "`nStart: build\crimson-host.exe --hv ; Start-VM Crimson-Era-System, Crimson-Era-Game"
}

<#
.SYNOPSIS
  Creates the Crimson Game OS and System OS partitions as Hyper-V VMs.

.DESCRIPTION
  Host OS     = this Windows install (the Hyper-V root partition). It runs
                crimson-host.exe, owns the controller and the screen.
  Game OS     = "Crimson-Game" VM: most cores, fixed memory, the bigger GPU
                partition. Plays the part of the Xbox One Exclusive partition.
  System OS   = "Crimson-System" VM: two cores, small fixed memory, a small GPU
                partition. Runs the shell (Home, Guide, Snap apps). Plays the
                part of the Xbox One Shared partition.

  Both VMs are differencing disks over one Windows VHDX you provide, so the
  pair costs little disk. Each gets crimson-agent.exe and a startup entry
  that connects back to the host over a Hyper-V socket.

  Needs: Windows 10/11 Pro, Enterprise or Education (Hyper-V), an elevated
  PowerShell, and a VHDX with Windows already installed (run Windows Setup
  once in a plain VM, or use any existing Windows VHDX).

.EXAMPLE
  .\New-CrimsonPartitions.ps1 -BaseVhdx D:\VMs\win11-base.vhdx
.EXAMPLE
  .\New-CrimsonPartitions.ps1 -BaseVhdx D:\VMs\win11-base.vhdx -NoGpu -GameCores 4 -GameMemoryGB 6
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $BaseVhdx,
    [string] $VmRoot = "$env:PUBLIC\Documents\Hyper-V\Crimson",
    [int]    $GameCores = 6,
    [int]    $GameMemoryGB = 8,
    [int]    $SystemCores = 2,
    [int]    $SystemMemoryGB = 3,
    [string] $SwitchName = "",
    [switch] $NoGpu,
    [switch] $NoDriverCopy,
    [switch] $Start
)
$ErrorActionPreference = 'Stop'
$ServiceId = 'c71b0e5a-3f2d-4e8b-9a61-7c0de0c41a57'   # must match CR_HV_SERVICE_STR in src/protocol.h
$AgentExe  = Join-Path $PSScriptRoot '..\build\crimson-agent.exe'

function Assert-Admin {
    $id = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not $id.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run this from an elevated PowerShell (Run as administrator).'
    }
}

function Assert-HyperV {
    $f = Get-WindowsOptionalFeature -Online -FeatureName Microsoft-Hyper-V-All -ErrorAction SilentlyContinue
    if (-not $f) { throw 'Hyper-V is not available on this edition of Windows (Home has no Hyper-V).' }
    if ($f.State -ne 'Enabled') {
        throw "Hyper-V is off. Turn it on with:`n  Enable-WindowsOptionalFeature -Online -FeatureName Microsoft-Hyper-V-All`nthen reboot and run this again."
    }
}

# The VMBus channel the partitions use to reach the host (AF_HYPERV sockets).
function Register-CrimsonService {
    $key = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Virtualization\GuestCommunicationServices\$ServiceId"
    if (-not (Test-Path $key)) { New-Item -Path $key -Force | Out-Null }
    Set-ItemProperty -Path $key -Name ElementName -Value 'Crimson partition link'
    Write-Host "Registered Hyper-V socket service $ServiceId"
}

# Host GPU driver files the guest needs for GPU partitioning (GPU-P). Same
# approach as Easy-GPU-PV: DriverStore files go to HostDriverStore, the rest
# to the same System32 path inside the guest.
function Copy-GpuDriver([string] $GuestWindows) {
    $gpu = Get-CimInstance Win32_VideoController | Where-Object { $_.PNPDeviceID -like 'PCI*' } | Select-Object -First 1
    if (-not $gpu) { Write-Warning 'No PCI GPU found to copy drivers from.'; return }
    Write-Host "Copying host driver for $($gpu.Name) into $GuestWindows"
    $files = Get-CimInstance Win32_PnPSignedDriverCIMDataFile |
        Where-Object { $_.Antecedent.DeviceID -eq $gpu.PNPDeviceID }
    foreach ($f in $files) {
        $src = $f.Dependent.Name
        if (-not (Test-Path $src)) { continue }
        if ($src -like '*\DriverStore\FileRepository\*') {
            $rel = $src.Substring($src.IndexOf('\DriverStore\FileRepository\') + 1)
            $dst = Join-Path $GuestWindows ('System32\Host' + $rel)
        } else {
            $dst = Join-Path $GuestWindows $src.Substring("$env:SystemRoot\".Length)
        }
        New-Item -ItemType Directory -Force -Path (Split-Path $dst) | Out-Null
        Copy-Item $src $dst -Force
    }
    # Copy each driver folder whole: some vendors load files the WMI list leaves out.
    $dirs = $files | ForEach-Object { $_.Dependent.Name } |
        Where-Object { $_ -like '*\DriverStore\FileRepository\*' } |
        ForEach-Object { ($_ -split '\\FileRepository\\')[1].Split('\')[0] } | Sort-Object -Unique
    foreach ($d in $dirs) {
        $src = "$env:SystemRoot\System32\DriverStore\FileRepository\$d"
        $dst = Join-Path $GuestWindows "System32\HostDriverStore\FileRepository\$d"
        New-Item -ItemType Directory -Force -Path $dst | Out-Null
        Copy-Item "$src\*" $dst -Recurse -Force
    }
}

# Puts the agent and its startup entry into a partition's disk.
function Install-Agent([string] $Vhdx, [string] $Role) {
    $disk = Mount-VHD -Path $Vhdx -PassThru | Get-Disk
    try {
        $part = Get-Partition -DiskNumber $disk.Number | Where-Object { $_.Type -eq 'Basic' } |
            Sort-Object Size -Descending | Select-Object -First 1
        if (-not $part.DriveLetter) { $part | Add-PartitionAccessPath -AssignDriveLetter; $part = Get-Partition -DiskNumber $disk.Number -PartitionNumber $part.PartitionNumber }
        $root = "$($part.DriveLetter):\"
        $crimson = Join-Path $root 'Crimson'
        New-Item -ItemType Directory -Force -Path $crimson | Out-Null
        Copy-Item $AgentExe $crimson -Force
        Copy-Item (Join-Path $PSScriptRoot 'Setup-Guest.ps1') $crimson -Force

        $agentArgs = if ($Role -eq 'game') {
            '--role game --name "Game OS" --hv --desktop --size 1920x1080 --fps 60'
        } else {
            '--role system --name "System OS" --hv --desktop --size 1920x1080 --fps 30 --guide-key F1'
        }
        $startup = Join-Path $root 'ProgramData\Microsoft\Windows\Start Menu\Programs\StartUp'
        New-Item -ItemType Directory -Force -Path $startup | Out-Null
        "@start `"`" /min C:\Crimson\crimson-agent.exe $agentArgs`r`n" |
            Set-Content -Path (Join-Path $startup 'crimson-agent.cmd') -Encoding ASCII
        Set-Content -Path (Join-Path $crimson 'role.txt') -Value $Role -Encoding ASCII

        if (-not $NoGpu -and -not $NoDriverCopy) { Copy-GpuDriver (Join-Path $root 'Windows') }
    } finally {
        Dismount-VHD -Path $Vhdx
    }
}

function New-Partition([string] $Name, [string] $Role, [int] $Cores, [int] $MemGB,
                       [int] $Reserve, [int] $Weight, [uint64] $GpuShare) {
    if (Get-VM -Name $Name -ErrorAction SilentlyContinue) {
        Write-Host "$Name already exists, leaving it as is"
        return
    }
    $dir  = Join-Path $VmRoot $Name
    $vhdx = Join-Path $dir "$Name.vhdx"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    New-VHD -Path $vhdx -ParentPath $BaseVhdx -Differencing | Out-Null
    Install-Agent $vhdx $Role

    $vm = New-VM -Name $Name -Generation 2 -MemoryStartupBytes ($MemGB * 1GB) -VHDPath $vhdx -Path $VmRoot
    Set-VM -VM $vm -StaticMemory -ProcessorCount $Cores -CheckpointType Disabled `
        -AutomaticStartAction Nothing -AutomaticStopAction ShutDown
    # Fixed slices like the console: the game is guaranteed its cores, the
    # system partition gets what it needs but can't starve the game.
    Set-VMProcessor -VM $vm -Reserve $Reserve -RelativeWeight $Weight
    Set-VMFirmware -VM $vm -EnableSecureBoot On
    if ($SwitchName) { Connect-VMNetworkAdapter -VMName $Name -SwitchName $SwitchName }

    if (-not $NoGpu) {
        # GPU partitioning: the same paravirtual GPU path WSL2 and Windows Sandbox use.
        Set-VM -VM $vm -GuestControlledCacheTypes $true -LowMemoryMappedIoSpace 1GB -HighMemoryMappedIoSpace 32GB
        Add-VMGpuPartitionAdapter -VM $vm
        $max = [uint64]1000000000
        $share = [uint64]($max * $GpuShare / 100)
        Set-VMGpuPartitionAdapter -VM $vm `
            -MinPartitionVRAM $share -MaxPartitionVRAM $share -OptimalPartitionVRAM $share `
            -MinPartitionEncode $share -MaxPartitionEncode $share -OptimalPartitionEncode $share `
            -MinPartitionDecode $share -MaxPartitionDecode $share -OptimalPartitionDecode $share `
            -MinPartitionCompute $share -MaxPartitionCompute $share -OptimalPartitionCompute $share
    }
    Write-Host "Created $Name ($Role): $Cores cores, $MemGB GB$(if (-not $NoGpu) { ", GPU share $GpuShare%" })"
}

Assert-Admin
Assert-HyperV
if (-not (Test-Path $BaseVhdx)) { throw "Base disk not found: $BaseVhdx" }
if (-not (Test-Path $AgentExe)) { throw "Build the agent first (crimson-agent.exe not found next to this script's ..\build)." }
if (-not $NoGpu -and -not (Get-VMHostPartitionableGpu -ErrorAction SilentlyContinue)) {
    Write-Warning 'This GPU does not offer partitioning. Continuing without GPU-P (software rendering in the partitions).'
    $NoGpu = $true
}

Register-CrimsonService
#                      name             role     cores        memory           reserve weight gpu%
New-Partition 'Crimson-Game'   'game'   $GameCores   $GameMemoryGB    100     1000   80
New-Partition 'Crimson-System' 'system' $SystemCores $SystemMemoryGB  10      200    20

Write-Host ''
Write-Host 'Next:'
Write-Host '  1. Start each VM once, sign in, and run C:\Crimson\Setup-Guest.ps1 as administrator'
Write-Host '     (auto sign-in, no sleep or lock screen, magenta desktop for the System OS).'
Write-Host '  2. On the host: build\crimson-host.exe --hv'
Write-Host '  3. Start-VM Crimson-System, Crimson-Game'
if ($Start) { Start-VM -Name 'Crimson-System', 'Crimson-Game' }

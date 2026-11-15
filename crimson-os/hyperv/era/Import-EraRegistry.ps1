<#
  Registers the donated services and COM classes in a mounted WinPE image by
  copying their keys from the donor image's hives. Called by
  New-CrimsonEraImage.ps1.
#>
param([string] $Donor, [string] $Target, [string] $Role, [switch] $Wow64)
$ErrorActionPreference = 'Stop'

function Load($name, $path) { & reg.exe load "HKLM\$name" $path | Out-Null; if ($LASTEXITCODE) { throw "reg load $path" } }
function Unload($name) { [gc]::Collect(); Start-Sleep -Milliseconds 300; & reg.exe unload "HKLM\$name" | Out-Null }
function Copy-Key($from, $to) {
    & reg.exe copy "HKLM\$from" "HKLM\$to" /s /f 2>$null | Out-Null
    if ($LASTEXITCODE) { Write-Warning "  not in donor: $from" }
}

Load CR_D_SYS "$Donor\Windows\System32\config\SYSTEM"
Load CR_T_SYS "$Target\Windows\System32\config\SYSTEM"
Load CR_D_SW  "$Donor\Windows\System32\config\SOFTWARE"
Load CR_T_SW  "$Target\Windows\System32\config\SOFTWARE"
try {
    foreach ($svc in 'hvsocket', 'vpci', 'Audiosrv', 'AudioEndpointBuilder', 'MMCSS') {
        Copy-Key "CR_D_SYS\ControlSet001\Services\$svc" "CR_T_SYS\ControlSet001\Services\$svc"
    }
    # Svchost groups are left as WinPE has them: they already list AudioSrv and
    # AudioEndpointBuilder, and overwriting them drops BFE, which kills networking.
    # multimedia scheduler classes (games register as "Games" for priority)
    Copy-Key 'CR_D_SW\Microsoft\Windows NT\CurrentVersion\Multimedia' 'CR_T_SW\Microsoft\Windows NT\CurrentVersion\Multimedia'
    Copy-Key 'CR_D_SW\Microsoft\Windows NT\CurrentVersion\Drivers32' 'CR_T_SW\Microsoft\Windows NT\CurrentVersion\Drivers32'
    # COM class for the audio device enumerator (XAudio2 2.9 needs no COM registration)
    Copy-Key 'CR_D_SW\Classes\CLSID\{BCDE0395-E52F-467C-8E3D-C4579291692E}' 'CR_T_SW\Classes\CLSID\{BCDE0395-E52F-467C-8E3D-C4579291692E}'
    # 32-bit SxS: Winners maps a requested version (common controls 6.0.0.0)
    # to the installed one, and WinPE lists only its 64-bit assemblies.
    if ($Wow64) {
        $w = 'Microsoft\Windows\CurrentVersion\SideBySide\Winners'
        $n = 0
        foreach ($k in Get-ChildItem "Registry::HKLM\CR_D_SW\$w" -ErrorAction SilentlyContinue) {
            if ($k.PSChildName -like 'x86_*' -or $k.PSChildName -like 'wow64_*') {
                Copy-Key "CR_D_SW\$w\$($k.PSChildName)" "CR_T_SW\$w\$($k.PSChildName)"
                $n++
            }
        }
        Write-Host "  registry: $n 32-bit SxS winners"
    }
} finally {
    Unload CR_D_SYS; Unload CR_T_SYS; Unload CR_D_SW; Unload CR_T_SW
}

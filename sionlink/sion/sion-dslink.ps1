# Puts Sion Link on the Sion and starts its DS-only network next to the home
# Wi-Fi. Then start Sion Link on the DS and pick "Sion".
# Password: the Sion's root password ("sion").
param([string]$Sion = '192.168.1.247')
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$opt = @('-o', 'StrictHostKeyChecking=accept-new')
Write-Host "Copying to the Sion at $Sion (enter its password if asked)..."
ssh @opt "root@$Sion" 'mkdir -p /tmp/sionlink'
scp @opt (Get-ChildItem "$here\files\*").FullName "root@${Sion}:/tmp/sionlink/"
if ($LASTEXITCODE -ne 0) { Write-Host 'Copy failed: is the Sion on and on the network?'; Read-Host 'Press Enter'; exit 1 }
ssh @opt "root@$Sion" 'sh /tmp/sionlink/install-sionlink.sh'
Read-Host 'Press Enter to close'

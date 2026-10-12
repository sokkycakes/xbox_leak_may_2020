@echo off
rem Installs the xbcompat build that times the Xbox vblank from the TV's
rem real refresh, plus the full-speed CPU setting, on the Raspberry Pi over
rem the network. The current build is backed up first; "Pi vblank undo.cmd"
rem puts it back. No reflash: /data (games, saves) isn't touched.
setlocal
set PI=192.168.1.128
set OPT=-o StrictHostKeyChecking=accept-new
echo [1/2] Sending the files and installing (password is xbox)...
rem The Pi has no SFTP server, so no scp: the files go over ssh as a tar stream.
tar -cf - -C "%~dp0." xbcompat-native.x86 libxbcompat_renderer.so.1 S02cpufreq clocks.sh vblank.sh | ssh %OPT% root@%PI% "rm -rf /tmp/push-vblank && mkdir -p /tmp/push-vblank && tar -xf - -C /tmp/push-vblank && sh /tmp/push-vblank/vblank.sh"
if errorlevel 1 (
  echo Something went wrong; vblank.txt below says what.
)
echo [2/2] Fetching the report (password once more)...
ssh %OPT% root@%PI% "cat /tmp/vblank.txt" > "%~dp0vblank.txt"
echo Done. Claude reads vblank.txt from this folder.
pause

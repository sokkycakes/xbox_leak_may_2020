@echo off
rem Puts back the xbcompat build that was on the Pi before "Pi vblank.cmd".
set PI=192.168.1.128
ssh -o StrictHostKeyChecking=accept-new root@%PI% "sh /tmp/push-vblank/vblank.sh undo || sh /root/vblank.sh undo"
pause

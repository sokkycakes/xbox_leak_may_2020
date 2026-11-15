@echo off
rem Process mode: host + two test partitions on this PC. No Hyper-V needed.
cd /d "%~dp0build"
start "" crimson-host.exe --windowed
timeout /t 1 >nul
start "Game OS partition" crimson-agent.exe --role game --name "test game" --test
start "System OS partition" crimson-agent.exe --role system --name "test shell" --test

@echo off
rem Process mode with the Blades dashboard (standalone-guide) as the System OS shell.
rem Usage: "Run Blades as System OS.cmd" "C:\path\to\Blades-Dashboard.exe" ["Game window title"]
rem The game partition captures the window whose title contains the 2nd argument,
rem or shows the test pattern when it is left out.
cd /d "%~dp0build"
if "%~1"=="" (
  echo Pass the path to Blades-Dashboard.exe
  exit /b 1
)
start "" "%~1"
start "" crimson-host.exe
timeout /t 2 >nul
start "System OS partition" crimson-agent.exe --role system --name "Blades" --window "Standalone XUI" --guide-key F1 --no-input
if "%~2"=="" (
  start "Game OS partition" crimson-agent.exe --role game --name "test game" --test
) else (
  start "Game OS partition" crimson-agent.exe --role game --name "%~2" --window "%~2" --follow-layout
)

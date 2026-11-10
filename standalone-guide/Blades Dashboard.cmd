@echo off
rem Opens the Xbox 360 Blades dashboard. Left/Right or LB/RB switch blades, F1 or the Xbox button opens the Guide (Backspace or B backs out, Y returns to the dashboard), F11 toggles fullscreen, Esc quits.
cd /d "%~dp0"
start "" "%~dp0build\Blades-Dashboard.exe"

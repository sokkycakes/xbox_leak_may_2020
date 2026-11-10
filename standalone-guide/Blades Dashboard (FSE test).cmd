@echo off
rem Desktop test of the FSE behaviour (full screen, Guide over other programs) without the Full Screen Experience.
cd /d "%~dp0build"
start "" Blades-Dashboard.exe --fse

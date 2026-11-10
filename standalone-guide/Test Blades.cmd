@echo off
rem Loads Blades scenes (converted copies) in probe mode and saves each log and screenshot to x360\results.
cd /d "%~dp0"
set B=file://%~dp0x360/blades-converted/
set B=%B:\=/%
if not exist x360\results mkdir x360\results
call :run dashmain "%B%dash_dashmain.xzp#dashmain.xur" "--dump-tree"
call :run dashmain-bootgames "%B%dash_dashmain.xzp#dashmain.xur" "--play=BootGames,EndBootGames"
call :run dashmain-bootmedia "%B%dash_dashmain.xzp#dashmain.xur" "--play=BootMediaCmd,EndBootMediaCmd"
call :run dashmain-2open "%B%dash_dashmain.xzp#dashmain.xur" "--play=2Open,2OpenEnd"
call :run gamesMetaGames "%B%dash_gamesbla.xzp#gamesMetaGames.xur"
call :run communityBar "%B%dash_live.xzp#CommunityBar.xur"
call :run systemBlade "%B%dash_dashskn1.xzp#BladeSkin.xur"
echo Done. Send the x360\results folder.
pause
exit /b
:run
del /q build\blades.log build\scene-preview.png 2>nul
start "" /wait build\Blades-Dashboard.exe --probe "--skin=%B%dash_dashuisk.xzp#skin.xur" "--scene=%~2" "%~3"
copy /y build\blades.log x360\results\%1.log >nul
if exist build\scene-preview.png copy /y build\scene-preview.png x360\results\%1.png >nul
exit /b

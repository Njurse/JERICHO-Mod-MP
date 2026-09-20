@echo off
rem Host a LAN game. Run this on the machine that hosts.
rem   PLAY_HOST.bat [port]        default port 1400
rem
rem The game opens; walk the menus to Las Vegas + Take a Ride and start. Run
rem PLAY_JOIN.bat on the other machine within the 30 s grace, or it will just
rem wait in the lobby.
setlocal
cd /d "%~dp0"
set "PORT=%~1"
if "%PORT%"=="" set "PORT=1400"
echo mp: hosting on port %PORT%. Choose Las Vegas + Take a Ride in the menus.
echo mp: allow inbound TCP and UDP on port %PORT% in Windows Firewall.
set "MP_AUTOSTART=host"
start "" "REDRIVER2_dev.exe" -nointro -nofmv -host %PORT%
endlocal

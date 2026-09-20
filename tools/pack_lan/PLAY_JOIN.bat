@echo off
rem Join a LAN game. Run this on the other machine.
rem   PLAY_JOIN.bat <host-ip>          e.g. PLAY_JOIN.bat 192.168.1.42
rem   PLAY_JOIN.bat 192.168.1.42:1400  ... on a non-default port
rem   PLAY_JOIN.bat                    host and client on THIS one machine
rem
rem Joining by address skips LAN discovery and needs only the host's TCP session
rem port open, so it is the path to trust when a firewall is in the way.
setlocal
cd /d "%~dp0"
set "ADDR=%~1"
if "%ADDR%"=="" set "ADDR=127.0.0.1"
rem must never be told to host as well
set "MP_AUTOSTART="
echo mp: joining %ADDR% ...
start "" "REDRIVER2_dev.exe" -nointro -nofmv -join %ADDR%
endlocal

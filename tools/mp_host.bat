@echo off
rem ============================================================================
rem mp_host.bat [port] [lobby] [dry]
rem
rem   Host a LAN game and go straight into hosting -- no frontend walking.
rem
rem       REDRIVER2_dev.exe -nointro -nofmv -host [port]      (default 1400)
rem
rem   MP_AUTOSTART=host is set for you, so the match starts as soon as a player is
rem   in. Pass "lobby" instead of a port to sit and wait for somebody to press
rem   Start. An MP_AUTOSTART you set yourself is always left alone.
rem
rem   The autostart matters more than it looks: the frontend's idle attract demo
rem   used to launch a demo level ~30 s in, blocking the host through the load and
rem   dropping everyone joining. mp suppresses that demo now, so without autostart
rem   an idle host waits quietly -- correct, but it means nothing starts by itself.
rem
rem   The other machine can join by address (mp_join.bat <this-ip>), which works
rem   whether or not LAN discovery does, and needs only the session port open.
rem
rem   NOTE: this uses start, so the game detaches and this window returns at once.
rem   There is no PID here to kill afterwards -- close the game window instead.
rem   mp_pair.bat is the one whose processes the harness can stop by itself.
rem ============================================================================
setlocal
set "EXEDIR=%~dp0\..\..\..\..\src_rebuild\bin\Release_dev"
if not "%MP_EXEDIR%"=="" set "EXEDIR=%MP_EXEDIR%"
if not exist "%EXEDIR%\REDRIVER2_dev.exe" (
echo echo mp: no REDRIVER2_dev.exe in "%EXEDIR%"
    echo     build it, or set MP_EXEDIR to the directory that has it
    exit /b 1
)
if /i "%~1"=="dry" goto :dry
set "PORT=%~1"
if "%PORT%"=="" set "PORT=1400"
if not defined MP_AUTOSTART set "MP_AUTOSTART=host"
if /i "%~2"=="lobby" set "MP_AUTOSTART="
echo mp: hosting on port %PORT%, autostart "%MP_AUTOSTART%"
cd /d "%EXEDIR%"
start "" "REDRIVER2_dev.exe" -nointro -nofmv -host %PORT%
endlocal
exit /b 0

:dry
set "PORT=%~2"
if "%PORT%"=="" set "PORT=1400"
if not defined MP_AUTOSTART set "MP_AUTOSTART=host"
echo mp: would run, in "%EXEDIR%"
echo   REDRIVER2_dev.exe -nointro -nofmv -host %PORT%
echo   with MP_AUTOSTART=%MP_AUTOSTART%
endlocal
exit /b 0

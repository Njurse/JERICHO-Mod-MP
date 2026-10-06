@echo off
rem ============================================================================
rem mp_join.bat [ip[:port]] [dry]
rem
rem   Join a LAN game and go straight into it -- no frontend walking.
rem
rem       JERICHO_dev.exe -nointro -nofmv -join [ip[:port]]
rem
rem       mp_join.bat 192.168.1.42          the host's LAN address
rem       mp_join.bat 192.168.1.42:1318     ... on a non-default port
rem       mp_join.bat                       host and client on this one machine
rem
rem   Leave the port off in the normal case: with a bare address the join uses
rem   THIS machine's mp.ini port, which is what the host is using too when you
rem   both run the same build. Add `:port` only if the host's mp.ini really says
rem   something else -- and remember that mp.ini is what the host LISTENS on, so
rem   a number here that disagrees with it cannot connect.
rem
rem   Joining by address skips LAN discovery entirely and needs only the host's TCP
rem   session port open, so it is the path to trust on a LAN with a firewall.
rem
rem   MP_AUTOSTART is cleared here: the client launches as soon as the host's start
rem   arrives, and it must never be told to host as well.
rem
rem   NOTE: this uses start, so the game detaches and this window returns at once.
rem   There is no PID here to kill afterwards -- close the game window instead.
rem ============================================================================
setlocal
set "EXEDIR=%~dp0\..\..\..\..\src_rebuild\bin\Release_dev"
if not "%MP_EXEDIR%"=="" set "EXEDIR=%MP_EXEDIR%"
if not exist "%EXEDIR%\JERICHO_dev.exe" (
echo echo mp: no JERICHO_dev.exe in "%EXEDIR%"
    echo     build it, or set MP_EXEDIR to the directory that has it
    exit /b 1
)
if /i "%~1"=="dry" goto :dry
set "ADDR=%~1"
if "%ADDR%"=="" set "ADDR=127.0.0.1"
set "MP_AUTOSTART="
echo mp: joining %ADDR%
cd /d "%EXEDIR%"
start "" "JERICHO_dev.exe" -nointro -nofmv -join %ADDR%
endlocal
exit /b 0

:dry
set "ADDR=%~2"
if "%ADDR%"=="" set "ADDR=127.0.0.1"
echo mp: would run, in "%EXEDIR%"
echo   JERICHO_dev.exe -nointro -nofmv -join %ADDR%
endlocal
exit /b 0

@echo off
rem ============================================================================
rem mp_dedi.bat [options]
rem
rem   A headless DEDICATED SERVER: hosts a real session with no game window, so a
rem   client has something to join, and so a server can be left up while the client
rem   side restarts. Ports and the beacon behave like a hosted game's.
rem
rem       mp_dedi.bat                    host on the default port
rem       mp_dedi.bat --port 1400        ... on another one
rem
rem   Ctrl-C stops it. It logs joins and leaves, which is the quickest way to tell a
rem   client that never connected from one that connected and was dropped.
rem ============================================================================
setlocal
set "EXEDIR=%~dp0\..\..\..\..\src_rebuild\bin\Release_dev"
if not "%MP_EXEDIR%"=="" set "EXEDIR=%MP_EXEDIR%"
if not exist "%EXEDIR%\REDRIVER2_dev.exe" (
echo echo mp: no REDRIVER2_dev.exe in "%EXEDIR%"
    echo     build it, or set MP_EXEDIR to the directory that has it
    exit /b 1
)
python "%~dp0mp_dediserver.py" %*
set RC=%ERRORLEVEL%
endlocal
exit /b %RC%

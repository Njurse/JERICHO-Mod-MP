@echo off
rem ============================================================================
rem mp_pair.bat [options]
rem
rem   Run TWO REAL instances on this one PC -- one hosting, one joining -- and print
rem   both sides' logs. Reach for this first: nearly every multiplayer bug so far
rem   showed up here, and the mock could never have found them because it only ever
rem   puts one real engine in the room.
rem
rem   Thin wrapper around mp_localpair.py, which does the work:
rem     * two throwaway run directories beside the game, built from directory
rem       junctions so the big trees are shared and nothing is copied. Separate
rem       working directories are the whole trick -- they are what stop the two
rem       REDRIVER2.log files and the two mp.ini files fighting;
rem     * the host instance gets MP_AUTOSTART=host, the client does not;
rem     * both executables are launched directly, so the PIDs are real and the
rem       cleanup stops exactly what it started -- unlike the launchers above,
rem       which use start and therefore cannot be cleaned up at all.
rem
rem       mp_pair.bat                      host + join, report, clean up
rem       mp_pair.bat --keep               leave the run dirs to read
rem       mp_pair.bat --settle 30 --seconds 80
rem       mp_pair.bat --clean              remove the run dirs again
rem
rem   Run --help for the rest.
rem ============================================================================
setlocal
set "EXEDIR=%~dp0\..\..\..\..\src_rebuild\bin\Release_dev"
if not "%MP_EXEDIR%"=="" set "EXEDIR=%MP_EXEDIR%"
if not exist "%EXEDIR%\REDRIVER2_dev.exe" (
echo echo mp: no REDRIVER2_dev.exe in "%EXEDIR%"
    echo     build it, or set MP_EXEDIR to the directory that has it
    exit /b 1
)
python "%~dp0mp_localpair.py" %*
set RC=%ERRORLEVEL%
if %RC% EQU 9009 echo mp: python is not on PATH
endlocal
exit /b %RC%

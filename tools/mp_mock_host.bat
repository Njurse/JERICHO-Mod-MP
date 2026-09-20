@echo off
rem ============================================================================
rem mp_mock_host.bat [options]
rem
rem   A headless mock HOST: speaks the mp protocol, spawns no game. Use it to drive
rem   the real game as a CLIENT without a second machine, and to create conditions
rem   on demand -- a rejection, a chosen build hash, a chosen city, a peer that goes
rem   quiet.
rem
rem       mp_mock_host.bat --start                  accept, then run a match
rem       mp_mock_host.bat --start --peer-pad 0x40  and drive the "host" car
rem       mp_mock_host.bat --reject mods            refuse the join, with a reason
rem       mp_mock_host.bat --port 1400 --city 1 --start
rem
rem   Start the game against it with mp_join.bat 127.0.0.1:<port>.
rem
rem   It is one-sided by nature: one real engine in the room, so it can prove the
rem   wire format and the client's own behaviour, never what two engines do to each
rem   other. mp_pair.bat is for that.
rem ============================================================================
setlocal
set "EXEDIR=%~dp0\..\..\..\..\src_rebuild\bin\Release_dev"
if not "%MP_EXEDIR%"=="" set "EXEDIR=%MP_EXEDIR%"
if not exist "%EXEDIR%\REDRIVER2_dev.exe" (
echo echo mp: no REDRIVER2_dev.exe in "%EXEDIR%"
    echo     build it, or set MP_EXEDIR to the directory that has it
    exit /b 1
)
echo mp: mock host -- point the game at it with mp_join.bat 127.0.0.1
python "%~dp0mp_test.py" host --start %*
set RC=%ERRORLEVEL%
endlocal
exit /b %RC%

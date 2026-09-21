@echo off
rem START_AGENT.bat -- run this ON THE OTHER PC, once, and leave the window open.
rem
rem After this, that machine is hands-free: it takes commands over the LAN
rem (sync / start / stop / log / status) and, when a new build arrives, it stops
rem the game, updates itself and starts the game again on its own.
rem
rem Put this file (and mp_agent.ps1) in the SAME FOLDER as REDRIVER2_dev.exe.
rem
rem Options, if you need them:
rem   START_AGENT.bat -Port 1401 -Token mysecret
setlocal
set "HERE=%~dp0"

if not exist "%HERE%REDRIVER2_dev.exe" (
    echo.
    echo   This folder has no REDRIVER2_dev.exe:
    echo     %HERE%
    echo   Put START_AGENT.bat and mp_agent.ps1 next to the game exe, then retry.
    echo.
    pause
    exit /b 1
)

echo.
echo   mp agent: this window must stay OPEN. Ctrl+C or closing it stops the agent.
echo.

rem Open the two ports, but only if we are ALREADY elevated -- adding a rule needs
rem administrator, and failing quietly here would surface later as "the other PC
rem cannot connect". Without this, Windows raises a prompt on the first listen,
rem which is easy to miss on a machine nobody is sitting at.
rem   * the agent's control port (1401), which is what a deploy talks to
rem   * the GAME port (1400), which is what carries the match itself
set "APORT=1401"

net session >nul 2>&1
if %errorlevel%==0 (
    netsh advfirewall firewall show rule name="JERICHO mp agent" >nul 2>&1
    if errorlevel 1 (
        netsh advfirewall firewall add rule name="JERICHO mp agent" dir=in action=allow protocol=TCP localport=%APORT% >nul 2>&1
        echo   firewall: allowed TCP %APORT% ^(the deploy agent^)
    )
    netsh advfirewall firewall show rule name="JERICHO mp game" >nul 2>&1
    if errorlevel 1 (
        netsh advfirewall firewall add rule name="JERICHO mp game" dir=in action=allow protocol=TCP localport=1400 >nul 2>&1
        echo   firewall: allowed TCP 1400 ^(the match itself^)
    )
) else (
    echo   note: not running as administrator, so firewall rules were NOT added.
    echo         If the other PC cannot reach this one, right-click this file and
    echo         Run as administrator once, then leave the window open.
)
echo.

powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%mp_agent.ps1" -Root "%HERE%." %*
echo.
echo   agent stopped.
pause

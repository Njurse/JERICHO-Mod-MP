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
echo   Allowing it through the firewall (TCP 1401) is what lets the other PC talk.
echo.

powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%mp_agent.ps1" -Root "%HERE%" %*
echo.
echo   agent stopped.
pause

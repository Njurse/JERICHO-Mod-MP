@echo off
rem START_AGENT.bat -- run this ON THE OTHER PC, once, and leave the window open.
rem
rem After this, that machine is hands-free: it takes commands (update / rollback /
rem start / stop / log / status) and, when told to update, it downloads that GitHub
rem release ITSELF, verifies it, stops the game, installs it and starts the game
rem again on its own. Nothing can be pushed to it.
rem
rem Put this file (and mp_agent.ps1) in the SAME FOLDER as JERICHO_dev.exe.
rem
rem By default the agent only listens on 127.0.0.1 (this PC). To drive it from the
rem other PC, give it THIS PC's LAN address:
rem   START_AGENT.bat -Bind 192.168.1.20
rem
rem The first run generates a random token, prints it, and keeps it in
rem mp_agent.config.json next to the game; pass it to mp_remote.py (--token).
rem
rem Other options, if you need them:
rem   START_AGENT.bat -Bind 192.168.1.20 -Port 1401
rem   START_AGENT.bat -Repo someone/JERICHO -Tag v0.9.1
rem
rem One-shot install of a release, no listener:
rem   powershell -NoProfile -ExecutionPolicy Bypass -File mp_agent.ps1 -InstallRelease -Tag alpha
setlocal
set "HERE=%~dp0"

if not exist "%HERE%JERICHO_dev.exe" (
    echo.
    echo   This folder has no JERICHO_dev.exe:
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
rem   * the agent's control port (1401), which is what mp_remote.py talks to
rem   * the GAME port (1400), which is what carries the match itself
rem Both rules are limited to the PRIVATE network profile and to addresses on the
rem local subnet, so a laptop on public Wi-Fi does not expose either port. A rule
rem left by an older START_AGENT.bat (any profile, any address) is replaced.
set "APORT=1401"

net session >nul 2>&1
if %errorlevel%==0 (
    netsh advfirewall firewall delete rule name="JERICHO mp agent" >nul 2>&1
    netsh advfirewall firewall add rule name="JERICHO mp agent" dir=in action=allow protocol=TCP localport=%APORT% profile=private remoteip=localsubnet >nul 2>&1
    echo   firewall: allowed TCP %APORT% ^(the agent^) - Private profile, local subnet only
    netsh advfirewall firewall delete rule name="JERICHO mp game" >nul 2>&1
    netsh advfirewall firewall add rule name="JERICHO mp game" dir=in action=allow protocol=TCP localport=1400 profile=private remoteip=localsubnet >nul 2>&1
    echo   firewall: allowed TCP 1400 ^(the match itself^) - Private profile, local subnet only
) else (
    echo   note: not running as administrator, so firewall rules were NOT added.
    echo         If the other PC cannot reach this one, right-click this file and
    echo         Run as administrator once, then leave the window open. The rules
    echo         only apply when this PC's network is set to Private.
)
echo.

powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%mp_agent.ps1" -Root "%HERE%." %*
echo.
echo   agent stopped.
pause

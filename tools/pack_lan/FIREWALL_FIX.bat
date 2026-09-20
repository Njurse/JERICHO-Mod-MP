@echo off
rem ============================================================================
rem FIREWALL_FIX.bat [port]        default port 1400 (matches PLAY_HOST.bat)
rem
rem Run this ON THE MACHINE THAT HOSTS, as ADMINISTRATOR (right-click this file
rem and choose "Run as administrator"). It prints the current state, then opens
rem ONLY the two ports the multiplayer uses:
rem
rem     TCP  the session connection every player makes to the host
rem     UDP  LAN discovery beacons (skipped entirely when joining by IP)
rem
rem It also allows REDRIVER2_dev.exe itself inbound, which is the rule Windows
rem creates when it first asks "allow this app through the firewall" -- if that
rem prompt was cancelled, Windows made a BLOCK rule instead, and that one wins
rem no matter what ports you open until it is removed (see below).
rem
rem Safe to run more than once.
rem ============================================================================
setlocal
set "PORT=%~1"
if "%PORT%"=="" set "PORT=1400"

net session >nul 2>&1
if errorlevel 1 (
    echo.
    echo   This needs an ADMINISTRATOR command prompt.
    echo   Right-click FIREWALL_FIX.bat and choose "Run as administrator".
    echo.
    pause
    exit /b 1
)

echo.
echo === network profile (Public blocks inbound by default) ===
powershell -NoProfile -Command "Get-NetConnectionProfile | Select-Object InterfaceAlias,NetworkCategory | Format-Table -AutoSize"

echo === existing REDRIVER2 firewall rules ===
powershell -NoProfile -Command "$r=Get-NetFirewallRule -DisplayName '*REDRIVER2*' -ErrorAction SilentlyContinue; if($r){$r|Select-Object DisplayName,Direction,Action,Enabled,Profile|Format-Table -AutoSize}else{'none'}"

echo === existing BLOCK rules mentioning REDRIVER2 (these override allows) ===
powershell -NoProfile -Command "$b=Get-NetFirewallRule -Direction Inbound -Action Block -ErrorAction SilentlyContinue | Where-Object { $_.DisplayName -like '*REDRIVER2*' }; if($b){$b|Select-Object DisplayName,Enabled,Profile|Format-Table -AutoSize}else{'none'}"

echo.
echo === adding the allow rules on port %PORT% ===
netsh advfirewall firewall delete rule name="REDRIVER2 mp session (TCP %PORT%)" >nul 2>&1
netsh advfirewall firewall delete rule name="REDRIVER2 mp discovery (UDP %PORT%)" >nul 2>&1
netsh advfirewall firewall delete rule name="REDRIVER2_dev inbound" >nul 2>&1

netsh advfirewall firewall add rule name="REDRIVER2_dev inbound" dir=in action=allow program="%~dp0REDRIVER2_dev.exe" enable=yes profile=any
netsh advfirewall firewall add rule name="REDRIVER2 mp session (TCP %PORT%)" dir=in action=allow protocol=TCP localport=%PORT% profile=any
netsh advfirewall firewall add rule name="REDRIVER2 mp discovery (UDP %PORT%)" dir=in action=allow protocol=UDP localport=%PORT% profile=any

echo.
echo Done. Re-run it for a different port:  FIREWALL_FIX.bat 1318
echo.
echo If REDRIVER2_dev.exe is in a DIFFERENT folder than this script, add the
echo program rule by hand, because Windows keys app rules to the full path.
echo.
pause
endlocal

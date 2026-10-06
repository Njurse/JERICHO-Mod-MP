@echo off
rem ============================================================================
rem mp_bot_client.bat [ip[:port]] [mode] [dry]
rem
rem   Start a SECOND instance on this machine that DRIVES ITSELF, so a session can
rem   be exercised with only ONE human at the wheel -- you drive the host, the bot
rem   chases you.
rem
rem       mp_bot_client.bat                    chase the host on this machine
rem       mp_bot_client.bat 192.168.1.42       ... on another machine's host
rem       mp_bot_client.bat "" pursuit         the two hunt EACH OTHER
rem       mp_bot_client.bat "" catmouse        the pair driven by the pathfinder
rem
rem   Give no port unless you changed mp.ini: it wins over a -host/-join port
rem   (MpConfigLoad runs after the cmdline parse), so the two ends only agree if
rem   they read the same mp.ini. Joining as a bare address is the safe form.
rem
rem   Modes (MP_BOT -- see MODS/mp/mp_bot.h):
rem     chase     the joiner CHASES. "the host flees, the joiner chases" is split by
rem               ROLE (mp_bot.c: flee = !fight && MpIsHost()), so this instance,
rem               being the joiner, chases whoever is driving the host. THAT IS THE
rem               ONE YOU WANT to be chased by.
rem     pursuit   mutual chase: both hunt, so they meet and collide.
rem     fight     both charge each other.
rem     catmouse  the same pair, driven by the pathfinder in ai/ (better round
rem               buildings than chase's straight-line primitive).
rem     random    a canned manoeuvre. No chasing at all.
rem
rem   Set the mode on THIS instance ONLY. Putting MP_BOT on the host as well makes
rem   the host FLEE -- which is correct for a bot-vs-bot run and wrong when you are
rem   the one driving it.
rem
rem   The host has to be up already (mp_host.bat, PLAY_HOST.bat, or your usual
rem   launch) with the session port open. This is mp_join.bat with MP_BOT set for
rem   you, and MP_AUTOSTART cleared so the client cannot try to host as well.
rem
rem   MP_BOT_DRAW=1 additionally draws the bot's aim/pathing on the client's HUD,
rem   which is how you watch it decide rather than guess.
rem
rem   NOTE: this uses start, so the game detaches and this window returns at once.
rem   There is no PID here to kill afterwards -- close the game window instead.
rem ============================================================================
setlocal
rem Works both in the package (this file next to the exe) and in the dev tree
rem (JERICHO\MODS\mp\tools\pack_lan -- five levels up to the repo root).
set "EXEDIR=%~dp0"
if not exist "%EXEDIR%JERICHO_dev.exe" set "EXEDIR=%~dp0\..\..\..\..\..\src_rebuild\bin\Release_dev"
if not "%MP_EXEDIR%"=="" set "EXEDIR=%MP_EXEDIR%"
if not exist "%EXEDIR%\JERICHO_dev.exe" (
    echo mp: no JERICHO_dev.exe in "%EXEDIR%"
    echo     build it, or set MP_EXEDIR to the directory that has it
    exit /b 1
)
if /i "%~1"=="dry" goto :dry
set "ADDR=%~1"
if "%ADDR%"=="" set "ADDR=127.0.0.1"
set "MODE=%~2"
if "%MODE%"=="" set "MODE=chase"
if not defined MP_BOT set "MP_BOT=%MODE%"
set "MP_AUTOSTART="
echo mp: bot client joining %ADDR% with MP_BOT=%MP_BOT%
cd /d "%EXEDIR%"
start "" "JERICHO_dev.exe" -nointro -nofmv -join %ADDR%
endlocal
exit /b 0

:dry
set "ADDR=%~2"
if "%ADDR%"=="" set "ADDR=127.0.0.1"
set "MODE=%~3"
if "%MODE%"=="" set "MODE=chase"
echo mp: would run, in "%EXEDIR%"
echo   JERICHO_dev.exe -nointro -nofmv -join %ADDR%
echo   with MP_BOT=%MODE%, MP_AUTOSTART cleared
endlocal
exit /b 0

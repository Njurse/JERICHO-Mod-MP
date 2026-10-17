@echo off
rem Build the LAN test package (JERICHO_mp_lan.7z in the project root).
rem
rem   make_lan_package.bat [output.7z]
rem
rem Ships the RELEASE build (bin\Release\JERICHO.exe): the public build, which
rem pre-includes carhacks + crumple + d1cars + mp. It is NOT the dev build -- a dev
rem build carries every mod and stays local. See premake5.lua JERICHO_RELEASE_MODS.rem
rem Ships the exe, the DLLs and JERICHO -- but NOT DRIVER2\ (the game data),
rem which is the player's own Driver 2 files and is not redistributed (the repo
rem copy is placeholders). The player drops their own DRIVER2\ next to the exe.
rem
rem DRIVER\D1CARS IS NOT SHIPPED, and that is deliberate: it is baked out of
rem Driver 1's copyrighted data. What IS shipped is the way to make it -- the
rem d1cars bake tools and BAKE_D1_CARS.bat / .sh at the package root, which reads the
rem player's OWN Driver 1 data from DRIVER\ and writes DRIVER\D1CARS\. The d1cars
rem MODULE is linked into the exe (JERICHO_RELEASE_MODS), so nothing else is
rem missing; carhacks offers a D1 city only once its DRIVER\D1CARS\<CITY>.LEV is
rem on disk (chkCityAssetsPresent), so before the bake the five cities simply are
rem not offered - the feature degrades to absent rather than to broken.
rem
rem The tools are copied out of the REPO below because the dev mirror EXCLUDES
rem d1cars (premake JER_MIRROR_EXCLUDE), so bin\Release never gets them.
setlocal
set "HERE=%~dp0"
set "ROOT=%HERE%..\..\..\..\.."
set "EXEDIR=%ROOT%\src_rebuild\bin\Release"
set "OUT=%~1"
if "%OUT%"=="" set "OUT=%ROOT%\JERICHO_mp_lan.7z"

set "SEVENZ=%ProgramFiles%\7-Zip\7z.exe"
if not exist "%SEVENZ%" set "SEVENZ=%ProgramFiles(x86)%\7-Zip\7z.exe"
if not exist "%SEVENZ%" (
    echo make_lan_package: 7-Zip not found. Install it, or run the two 7z
    echo commands in this file by hand.
    exit /b 1
)

echo packaging %EXEDIR% -^> %OUT%

rem The bake tools, out of the repo: the dev mirror excludes d1cars, so the exe
rem dir has no copy of them and the package would ship without a way to bake.
if exist "%ROOT%\JERICHO\MODS\d1cars\tools" (
    xcopy /E /I /Y "%ROOT%\JERICHO\MODS\d1cars\tools" "%EXEDIR%\JERICHO\MODS\d1cars\tools" >nul
) else (
    echo make_lan_package: no JERICHO\MODS\d1cars\tools in the repo - the package
    echo                  will ship without the BAKE_D1_CARS tools. See the
    echo                  d1cars submodule ^(JERICHO-D1^).
)

pushd "%EXEDIR%" || exit /b 1
rem -xr! excludes are the "never ship car art or game data" rule: JERICHO\ is the
rem mod mirror and can never carry a car-data bank (BC*.LEV/.LCF, BetterCars) or a
rem *.sparse.bak a local bankfill left behind; DRIVER2\ is the player's own data.
"%SEVENZ%" a -t7z -mx=5 "-xr!FMV" "-xr!BetterCars" "-xr!BC*.LEV" "-xr!BC*.LCF" "-xr!*.sparse.bak" "%OUT%" ^
    JERICHO.exe SDL2.dll OpenAL32.dll soft_oal.dll config.ini ^
    VERSION.txt JERICHO
popd

pushd "%HERE%" || exit /b 1
"%SEVENZ%" a -t7z -mx=5 "%OUT%" PLAY_HOST.bat PLAY_JOIN.bat mp_bot_client.bat FIREWALL_FIX.bat BAKE_D1_CARS.bat BAKE_D1_CARS.sh update_jericho.bat update_jericho.sh README_LAN.txt README_LAN_TEST.txt
popd

rem The remote-testing agent, staged at the package ROOT (next to the exe) so the
rem other PC gets it by unpacking -- that is what makes it a fixture you set up once
rem and then stop thinking about: START_AGENT.bat there, and this machine can have
rem it install a GitHub release (it downloads and verifies the build itself; nothing
rem is pushed to it), start either seat, and pull both logs.
pushd "%HERE%..\remote" || exit /b 1
"%SEVENZ%" a -t7z -mx=5 "%OUT%" mp_agent.ps1 START_AGENT.bat README_REMOTE.txt
popd

rem The DEDICATED SERVER, staged the same way and for the same reason: the dev
rem mirror strips a mod's tools\ directory, so bin\Release has none of it. A
rem release ships the dedi so a match can be left running, and ships NO master
rem discovery service (that is the shared one every install points at), so the
rem copy below leaves masterserver\ out and then proves it did.
set "SRVSTAGE=%TEMP%\mp_lan_servers"
if exist "%SRVSTAGE%" rd /s /q "%SRVSTAGE%"
mkdir "%SRVSTAGE%\JERICHO\MODS\mp\tools\servers" 2>nul
copy /y "%ROOT%\JERICHO\MODS\mp\tools\mp_test.py" "%SRVSTAGE%\JERICHO\MODS\mp\tools\" >nul
xcopy /Y /I "%ROOT%\JERICHO\MODS\mp\tools\servers\*.py" "%SRVSTAGE%\JERICHO\MODS\mp\tools\servers\" >nul
xcopy /Y /I "%ROOT%\JERICHO\MODS\mp\tools\servers\*.md" "%SRVSTAGE%\JERICHO\MODS\mp\tools\servers\" >nul
xcopy /Y /I "%ROOT%\JERICHO\MODS\mp\tools\servers\run.*" "%SRVSTAGE%\JERICHO\MODS\mp\tools\servers\" >nul
xcopy /E /I /Y "%ROOT%\JERICHO\MODS\mp\tools\servers\dediserver" "%SRVSTAGE%\JERICHO\MODS\mp\tools\servers\dediserver\" >nul
if exist "%SRVSTAGE%\JERICHO\MODS\mp\tools\servers\masterserver" (
    echo make_lan_package: refusing to package the master server - it is the shared
    echo                  discovery service, not something to hand out.
    exit /b 1
)
pushd "%SRVSTAGE%" || exit /b 1
"%SEVENZ%" a -t7z -mx=5 "%OUT%" JERICHO\MODS\mp\tools\servers JERICHO\MODS\mp\tools\mp_test.py "-xr!__pycache__"
popd

rem Ship an mp.ini that REFUSES a build mismatch instead of half-working. The
rem package is always copied whole, so the two machines' digests match; if someone
rem later replaces only the exe on one side, the join is refused with a clear
rem message rather than the two players silently failing to see each other.
rem Staged in TEMP and added with its relative path, so the DEV machine's own
rem JERICHO\CONFIG\mp.ini is never touched (packaging must not change the build).
set "STAGE=%TEMP%\mp_lan_stage"
if exist "%STAGE%" rd /s /q "%STAGE%"
mkdir "%STAGE%\JERICHO\CONFIG" 2>nul
> "%STAGE%\JERICHO\CONFIG\mp.ini" echo # LAN build (JERICHO mp) -- copied whole, so the digests match.
>> "%STAGE%\JERICHO\CONFIG\mp.ini" echo # strict_version = 1 REFUSES a join when the other machine runs a different build.
>> "%STAGE%\JERICHO\CONFIG\mp.ini" echo strict_version = 1
rem The session port has to be written down here, not left to MP_DEFAULT_PORT: a
rem config `port` WINS over a -host/-join port, so with no line here the host
rem listened on 1318 while PLAY_HOST/PLAY_JOIN and the firewall rule in
rem README_LAN all said 1400 -- a join by address could never connect. Keep these
rem three in step: this line, PLAY_HOST's default, and README_LAN's firewall note.
>> "%STAGE%\JERICHO\CONFIG\mp.ini" echo port = 1400
rem No modlist is staged here, and none ships: a Release build DELETES
rem bin/Release/JERICHO/CONFIG/modlist.ini rather than writing one, so installing
rem this package can never replace a machine's own module toggles. Each module then
rem runs from its mod.toml default, which jer_registry.c bakes in.
pushd "%STAGE%" || exit /b 1
"%SEVENZ%" a -t7z -mx=5 "%OUT%" JERICHO\CONFIG\mp.ini
popd

echo done: %OUT%
endlocal

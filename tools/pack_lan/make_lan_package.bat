@echo off
rem Build the LAN test package (REDRIVER2_mp_lan.7z in the project root).
rem
rem   make_lan_package.bat [output.7z]
rem
rem Ships the exe, the DLLs, the game data and JERICHO -- but NOT the 1.5 GB of
rem FMV videos, which are the bulk of the build and are not needed by the
rem launchers (they pass -nofmv, which the game supports). Add DRIVER2\FMV to
rem the second 7z command if someone wants the movies.
setlocal
set "HERE=%~dp0"
set "ROOT=%HERE%..\..\..\..\.."
set "EXEDIR=%ROOT%\src_rebuild\bin\Release_dev"
set "OUT=%~1"
if "%OUT%"=="" set "OUT=%ROOT%\REDRIVER2_mp_lan.7z"

set "SEVENZ=%ProgramFiles%\7-Zip\7z.exe"
if not exist "%SEVENZ%" set "SEVENZ=%ProgramFiles(x86)%\7-Zip\7z.exe"
if not exist "%SEVENZ%" (
    echo make_lan_package: 7-Zip not found. Install it, or run the two 7z
    echo commands in this file by hand.
    exit /b 1
)

echo packaging %EXEDIR% -^> %OUT%
pushd "%EXEDIR%" || exit /b 1
"%SEVENZ%" a -t7z -mx=5 "-xr!FMV" "%OUT%" ^
    REDRIVER2_dev.exe SDL2.dll OpenAL32.dll soft_oal.dll config.ini ^
    VERSION.txt DRIVER2 JERICHO
popd

pushd "%HERE%" || exit /b 1
"%SEVENZ%" a -t7z -mx=5 "%OUT%" PLAY_HOST.bat PLAY_JOIN.bat FIREWALL_FIX.bat README_LAN.txt
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
pushd "%STAGE%" || exit /b 1
"%SEVENZ%" a -t7z -mx=5 "%OUT%" JERICHO\CONFIG\mp.ini
popd

echo done: %OUT%
endlocal

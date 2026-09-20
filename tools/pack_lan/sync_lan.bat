@echo off
rem Build AND package the LAN test build in ONE step, stamped with the build hash.
rem
rem   sync_lan.bat
rem
rem Produces  REDRIVER2_mp_lan_<build>.7z  in the project root. Copy that ONE file
rem to the other PC and extract it over the existing folder -- no Visual Studio,
rem no git and no dependencies are needed there.
rem
rem <build> is the same string the game logs at startup
rem   [mp] multiplayer ready (... build xxxx, mods yyyy)
rem and prints on the pause-menu scoreboard, so you can tell at a glance whether
rem the two machines are running the same build.
setlocal
set "HERE=%~dp0"
rem Normalise the repo root with pushd -- a path containing ".." breaks the
rem `cd /d "%~dp0"` inside build_dev.bat (its trailing backslash escapes the quote).
pushd "%HERE%..\..\..\..\.." || exit /b 1
set "ROOT=%CD%"
popd
set "SRC=%ROOT%\src_rebuild"
set "EXEDIR=%SRC%\bin\Release_dev"

rem ---------------------------------------------------------------- 1) stamp
rem The SAME value premake bakes in as JERICHO_BUILD_VERSION, so the package name,
rem VERSION.txt and what the game reports all agree.
set "BUILD="
for /f "delims=" %%v in ('git -C "%ROOT%" describe --tags --always --dirty 2^>nul') do set "BUILD=%%v"
if "%BUILD%"=="" set "BUILD=unknown"
echo [sync_lan] build stamp: %BUILD%

rem ------------------------------------------------------- 2) regenerate vcxproj
rem premake bakes JERICHO_BUILD_VERSION into the project, so it must run BEFORE the
rem build or the stamp would be a release behind.
echo [sync_lan] regenerating project files (premake5 vs2019)...
pushd "%SRC%" || exit /b 1
set "SDL2_DIR=%SRC%\dependencies\SDL2-2.30.2"
set "OPENAL_DIR=%SRC%\dependencies\openal-soft-1.23.1-bin"
set "JPEG_DIR=%SRC%\dependencies\jpeg-9d"
premake5 vs2019 >nul
if errorlevel 1 (
    echo [sync_lan] premake FAILED
    popd & exit /b 1
)
popd

rem ------------------------------------------------------------------- 3) build
rem build_dev.bat passes msbuild a RELATIVE path (build\REDRIVER2.vcxproj), so it
rem must be invoked with src_rebuild as the current directory.
echo [sync_lan] building Release_dev (this takes a few minutes)...
pushd "%SRC%" || exit /b 1
call "%SRC%\build_dev.bat"
set "RC=%ERRORLEVEL%"
popd
if not "%RC%"=="0" (
    echo [sync_lan] BUILD FAILED - nothing was packaged
    exit /b 1
)

rem ------------------------------------------------------------------ 4) stamp in
> "%EXEDIR%\VERSION.txt" echo %BUILD%

rem ------------------------------------------------------------------ 5) package
call "%HERE%make_lan_package.bat" "%ROOT%\REDRIVER2_mp_lan_%BUILD%.7z"
if errorlevel 1 (
    echo [sync_lan] packaging FAILED
    exit /b 1
)

echo.
echo [sync_lan] done.  Copy this ONE file to the other PC and extract it over the
echo            game folder:  %ROOT%\REDRIVER2_mp_lan_%BUILD%.7z
echo            Then check both games report "build" %BUILD% (startup log, or the
echo            pause-menu scoreboard).
endlocal

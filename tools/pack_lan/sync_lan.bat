@echo off
rem Build AND package the LAN test build in ONE step, stamped with the build hash.
rem
rem   sync_lan.bat
rem
rem Produces  JERICHO_mp_lan_<build>.7z  in the project root. Copy that ONE file
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
set "EXEDIR=%SRC%\bin\Release"

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
rem The RELEASE build, not the dev one: a release pre-includes only carhacks +
rem crumple + mp (premake5.lua JERICHO_RELEASE_MODS), and bin\Release is what this
rem package ships.
rem
rem exports.def is generated from a LINKER MAP, and the committed copy is shaped
rem for the DEV build -- it names every mod's entry symbol. A release links only
rem the three, so that def fails the link with LNK2001s. So: link once against a
rem throwaway empty def (that pass is what writes the map), regenerate the def
rem from that map, then relink. The CI workflow does the same. The committed,
rem dev-shaped def is restored afterwards, so the repo is left as it was found.
rem
rem build_jericho.bat passes msbuild a RELATIVE path (build\JERICHO.vcxproj), so
rem it must be invoked with src_rebuild as the current directory.
echo [sync_lan] building Release (this takes a few minutes)...
pushd "%SRC%" || exit /b 1
copy /Y "%SRC%\exports.def" "%SRC%\exports.def.keep" >nul
> "%SRC%\exports.def" echo EXPORTS
call "%SRC%\build_jericho.bat"
if errorlevel 1 (
    copy /Y "%SRC%\exports.def.keep" "%SRC%\exports.def" >nul & del "%SRC%\exports.def.keep" >nul
    echo [sync_lan] BUILD FAILED (pass 1) - nothing was packaged
    popd & exit /b 1
)
"%SRC%\bin\Release\gen_exports.exe" "%SRC%\bin\Release\JERICHO.map" "%SRC%\exports.def"
if errorlevel 1 (
    copy /Y "%SRC%\exports.def.keep" "%SRC%\exports.def" >nul & del "%SRC%\exports.def.keep" >nul
    echo [sync_lan] gen_exports FAILED - nothing was packaged
    popd & exit /b 1
)
call "%SRC%\build_jericho.bat"
if errorlevel 1 (
    copy /Y "%SRC%\exports.def.keep" "%SRC%\exports.def" >nul & del "%SRC%\exports.def.keep" >nul
    echo [sync_lan] BUILD FAILED (pass 2) - nothing was packaged
    popd & exit /b 1
)
copy /Y "%SRC%\exports.def.keep" "%SRC%\exports.def" >nul
del "%SRC%\exports.def.keep" >nul
popd

rem ------------------------------------------------------------------ 4) stamp in
> "%EXEDIR%\VERSION.txt" echo %BUILD%

rem ------------------------------------------------------------------ 5) package
call "%HERE%make_lan_package.bat" "%ROOT%\JERICHO_mp_lan_%BUILD%.7z"
if errorlevel 1 (
    echo [sync_lan] packaging FAILED
    exit /b 1
)

echo.
echo [sync_lan] done.  Copy this ONE file to the other PC and extract it over the
echo            game folder:  %ROOT%\JERICHO_mp_lan_%BUILD%.7z
echo            Then check both games report "build" %BUILD% (startup log, or the
echo            pause-menu scoreboard).
endlocal

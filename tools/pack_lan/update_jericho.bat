@echo off
rem ============================================================================
rem  update_jericho.bat -- update this JERICHO install from the latest RELEASE.
rem
rem  Run it from the folder the game is installed in, or just double-click it
rem  there. It fetches the newest published release -- NOT the rolling "alpha"
rem  pre-release, which is a build of whatever is on main right now and is not
rem  meant to be played -- verifies its checksum, and copies it over this
rem  install.
rem
rem  WHAT IT WILL NOT TOUCH
rem    CONFIG\   your module toggles and settings: modlist.ini, carhacks.ini,
rem              mp.ini, crumple.ini, config.ini. Never written.
rem    DRIVER\   your baked Driver 1 car data, if you have any. Never written.
rem    MODS\<x>\ any module you added yourself. Files are only ever overwritten,
rem              never deleted, so nothing of yours can disappear.
rem  It only ever overwrites files the release itself carries.
rem
rem  USAGE
rem    update_jericho.bat          the latest release
rem    update_jericho.bat dev      the latest release's Release_dev build, which
rem                                carries every module and prints more
rem
rem  NEEDS: curl and tar, both of which ship with Windows 10 and later.
rem ============================================================================
setlocal
cd /d "%~dp0"

set "REPO=Njurse/JERICHO"
set "ASSET=JERICHO_Release_win64.zip"
set "KIND=release"
if /i "%~1"=="dev" set "ASSET=JERICHO_Release_dev_win64.zip"
if /i "%~1"=="dev" set "KIND=release dev"

rem releases/latest/download resolves to the newest NON-prerelease release, so
rem alpha is excluded without asking the API and without parsing any JSON.
set "BASE=https://github.com/%REPO%/releases/latest/download"
set "TMP=%TEMP%\jericho_update_%RANDOM%"

echo update_jericho: updating this install from the latest %KIND%
echo   folder: %CD%
echo.

where curl >nul 2>nul
if errorlevel 1 (
	echo   FAILED: no curl on PATH.
	echo   Windows 10 and later ship it as C:\Windows\System32\curl.exe. If yours
	echo   does not have it, download the release by hand from
	echo   https://github.com/%REPO%/releases/latest
	goto :fail
)

rem Windows ships tar as System32\tar.exe (bsdtar). Prefer it BY FULL PATH: a
rem MSYS/Cygwin tar earlier on PATH is GNU tar, which reads the "C:" of a Windows
rem path as a remote host name and fails with "Cannot connect to C: resolve
rem failed".
set "TAR=%SystemRoot%\System32\tar.exe"
if not exist "%TAR%" set "TAR=tar"
"%TAR%" --version >nul 2>&1
if errorlevel 1 (
	echo   FAILED: no usable tar.
	echo   Windows 10 and later ship it as C:\Windows\System32\tar.exe. Without it
	echo   this cannot unpack the release; download and unzip it by hand from
	echo   https://github.com/%REPO%/releases/latest
	goto :fail
)

mkdir "%TMP%" 2>nul

echo [1/5] downloading %ASSET% ...
curl -fsSL -o "%TMP%\%ASSET%" "%BASE%/%ASSET%"
if errorlevel 1 (
	echo   FAILED: could not download %ASSET%.
	echo   Check your connection, or that a release exists at
	echo   https://github.com/%REPO%/releases/latest
	goto :fail
)
for %%F in ("%TMP%\%ASSET%") do echo       %%~zF bytes

echo [2/5] downloading SHA256SUMS ...
curl -fsSL -o "%TMP%\SHA256SUMS" "%BASE%/SHA256SUMS"
if errorlevel 1 (
	echo   FAILED: could not download SHA256SUMS, so the download cannot be
	echo   verified. Nothing was changed.
	goto :fail
)

echo [3/5] verifying the checksum ...
set "WANT="
set "GOT="
for /f "tokens=1" %%A in ('findstr /i /c:"%ASSET%" "%TMP%\SHA256SUMS"') do set "WANT=%%A"
for /f "skip=1 tokens=*" %%H in ('certutil -hashfile "%TMP%\%ASSET%" SHA256') do if not defined GOT set "GOT=%%H"
set "GOT=%GOT: =%"
if not defined WANT (
	echo   FAILED: SHA256SUMS has no line for %ASSET%. Nothing was changed.
	goto :fail
)
if /i not "%WANT%"=="%GOT%" (
	echo   FAILED: checksum mismatch -- the download is not the published file.
	echo     published: %WANT%
	echo     yours:     %GOT%
	echo   Nothing was changed. Try again, or download by hand from
	echo   https://github.com/%REPO%/releases/latest
	goto :fail
)
echo       ok: %GOT%

echo [4/5] unpacking ...
mkdir "%TMP%\x" 2>nul
"%TAR%" -xf "%TMP%\%ASSET%" -C "%TMP%\x"
if errorlevel 1 (
	echo   FAILED: could not unpack the release. Nothing was changed.
	goto :fail
)

rem The one directory that must never be written. robocopy /XD takes a full path.
set "KEEPCONFIG=%TMP%\x\JERICHO\CONFIG"

echo [5/5] copying over this install, leaving CONFIG\ and DRIVER\ alone ...
robocopy "%TMP%\x" "%CD%" /E /XD "%KEEPCONFIG%" /NP /NFL /NDL /NJH /NJS /R:1 /W:1
rem robocopy reports 0..7 for success and 8+ for a real failure.
if errorlevel 8 (
	echo   FAILED: robocopy could not finish. Some files were NOT updated.
	goto :fail
)

echo.
echo update_jericho: done.
echo   updated:  the game, JERICHO\ and its released modules
echo   kept:     CONFIG\  (your toggles and settings)
echo             DRIVER\  (your baked Driver 1 car data)
echo             any module you added under MODS\
echo.
echo Note: files are overwritten, never deleted, so an old file this release no
echo longer ships may still be here. That is what keeps your own mods safe.
echo Turn crumple and Caine's Crossfire on in Options - JERICHO if you want them.
pause
rmdir /s /q "%TMP%" 2>nul
exit /b 0

:fail
echo.
echo update_jericho: FAILED -- see above. Nothing was left half-applied.
pause
rmdir /s /q "%TMP%" 2>nul
exit /b 1

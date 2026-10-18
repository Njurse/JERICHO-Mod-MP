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
rem    config.ini  your window size and fullscreen settings. This is the one at the
rem              install root (the release ships a default), NOT CONFIG\config.ini.
rem              Never replaced: a key the release adds that yours lacks is appended,
rem              and every value you already have is left alone.
rem    DRIVER\   your baked Driver 1 car data, if you have any. Never written.
rem    DRIVER2\  your own game data (levels, models, cutscenes). Never written.
rem    MODS\<x>\ any module you added yourself. Files are only ever overwritten,
rem              never deleted, so nothing of yours can disappear.
rem  It only ever overwrites files the release itself carries.
rem
rem  USAGE
rem    update_jericho.bat          the latest release, in this install's flavour
rem                                (Release_dev if JERICHO\CONFIG\installed.json
rem                                says so, else Release)
rem    update_jericho.bat dev      the latest release's Release_dev build, which
rem                                carries every module and prints more
rem    update_jericho.bat release  the plain Release build, whatever is installed
rem
rem  The game's "Update JERICHO (Restart)" button runs it with these as well:
rem    --tag vX.Y.Z      download that release (the one the game showed) instead
rem                      of whatever "latest" is by the time this runs
rem    --wait PID        wait (up to a minute) for the game to exit before copying
rem    --relaunch EXE    start the game again afterwards (also after a failure:
rem                      nothing was changed, so the old one still runs)
rem
rem  After a successful update it records what was installed in
rem  JERICHO\CONFIG\installed.json (tag, asset, SHA256): the game compares that
rem  SHA256 with the release's to notice a re-cut tag, and reads the asset name to
rem  keep the flavour.
rem
rem  NEEDS: curl and tar, both of which ship with Windows 10 and later.
rem ============================================================================
rem Run from a COPY in %TEMP%. The release carries this script too, and cmd reads
rem a batch file as it goes, so robocopy overwriting it mid-run would derail the
rem rest. The copy is told where the install is (--here). If the copy cannot be
rem made it simply runs in place, as it always did.
if /i not "%~dp0"=="%TEMP%\" (
	copy /y "%~f0" "%TEMP%\jericho_update_self.bat" >nul 2>nul && "%TEMP%\jericho_update_self.bat" --here "%~dp0." %*
)
setlocal
cd /d "%~dp0"

set "REPO=Njurse/JERICHO"
set "FLAVOUR="
set "TAG="
set "WAITPID="
set "RELAUNCH="
:args
if "%~1"=="" goto argsdone
if /i "%~1"=="--here" (cd /d "%~2" & shift & shift & goto args)
if /i "%~1"=="dev" (set "FLAVOUR=dev" & shift & goto args)
if /i "%~1"=="release" (set "FLAVOUR=release" & shift & goto args)
if /i "%~1"=="--tag" (set "TAG=%~2" & shift & shift & goto args)
if /i "%~1"=="--wait" (set "WAITPID=%~2" & shift & shift & goto args)
if /i "%~1"=="--relaunch" (set "RELAUNCH=%~2" & shift & shift & goto args)
echo update_jericho: ignoring unknown argument %1
shift
goto args
:argsdone

rem No flavour asked for: keep the one this install was last updated to.
if not defined FLAVOUR if exist "JERICHO\CONFIG\installed.json" (
	findstr /c:"Release_dev" "JERICHO\CONFIG\installed.json" >nul 2>nul && set "FLAVOUR=dev"
)
set "ASSET=JERICHO_Release_win64.zip"
set "KIND=release"
if /i "%FLAVOUR%"=="dev" set "ASSET=JERICHO_Release_dev_win64.zip"
if /i "%FLAVOUR%"=="dev" set "KIND=release dev"

rem releases/latest/download resolves to the newest NON-prerelease release, so
rem alpha is excluded without asking the API and without parsing any JSON. With
rem --tag (the game's button) it is that exact release.
set "BASE=https://github.com/%REPO%/releases/latest/download"
if defined TAG set "BASE=https://github.com/%REPO%/releases/download/%TAG%"
if defined TAG set "KIND=%KIND% %TAG%"
set "TMP=%TEMP%\jericho_update_%RANDOM%"

if defined WAITPID call :waitgame

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

rem Directories that must never be written. robocopy /XD takes a full path.
rem DRIVER\ and DRIVER2\ are absent from a release already (nothing to copy), but
rem the /XD belt keeps them safe if one ever ships data there again.
set "KEEPCONFIG=%TMP%\x\JERICHO\CONFIG"
set "KEEPDRIVER=%TMP%\x\DRIVER"
set "KEEPDRIVER2=%TMP%\x\DRIVER2"
rem The ROOT config.ini holds the player's own window size and fullscreen, and the
rem release carries a default one (from data\). /XF names that one file; CONFIG\
rem is already kept whole above, so nothing else called config.ini is affected.
set "KEEPINISRC=%TMP%\x\config.ini"
rem A release now ships config.default.ini instead (build.yml), which is safe to
rem overwrite; an older release still carries config.ini, hence the /XF above.
rem The DEDICATED SERVER's config is the OPERATOR's -- port, name, city, player cap,
rem whether it beacons -- so it is the third thing an update must not overwrite.
rem Excluded from the copy like config.ini, and installed just below ONLY when this
rem machine has none: the release's copy is a default, not a replacement.
set "KEEPDEDI=%TMP%\x\JERICHO\MODS\mp\tools\servers\dediserver\dediserver.json"

echo [5/5] copying over this install, leaving CONFIG\, config.ini, dediserver.json, DRIVER\ and DRIVER2\ alone ...
robocopy "%TMP%\x" "%CD%" /E /XD "%KEEPCONFIG%" "%KEEPDRIVER%" "%KEEPDRIVER2%" /XF "%KEEPINISRC%" "%KEEPDEDI%" /NP /NFL /NDL /NJH /NJS /R:1 /W:1
rem robocopy reports 0..7 for success and 8+ for a real failure.
if errorlevel 8 (
	echo   FAILED: robocopy could not finish. Some files were NOT updated.
	goto :fail
)

rem The dedicated server's default config, installed ONCE: an operator who has one
rem keeps it exactly as it is, and a fresh install still gets a file to edit. Newer
rem releases ship only dediserver.example.json (the server creates dediserver.json
rem from it on first start), so this only fires for an older package carrying one.
if not exist "%CD%\JERICHO\MODS\mp\tools\servers\dediserver\dediserver.json" (
	if exist "%KEEPDEDI%" (
		if not exist "%CD%\JERICHO\MODS\mp\tools\servers\dediserver" mkdir "%CD%\JERICHO\MODS\mp\tools\servers\dediserver"
		copy /y "%KEEPDEDI%" "%CD%\JERICHO\MODS\mp\tools\servers\dediserver\dediserver.json" >nul
	)
)

rem config.ini was not copied, so you keep every value you set. But the release may carry a
rem key yours has never seen, so add just those, under their sections. merge_config.ps1 ships
rem with the release (unpacked into %TMP%\x) and this is best-effort: a release too old to
rem carry it simply skips the step. The game's ini reader takes the FIRST match for a
rem section+key, so nothing appended here can override a value of yours.
set "JERICHO_CFG_REL=%TMP%\x\config.ini"
if exist "%TMP%\x\config.default.ini" set "JERICHO_CFG_REL=%TMP%\x\config.default.ini"
set "JERICHO_CFG_USR=%CD%\config.ini"
rem No config.ini yet: the game seeds one from config.default.ini on start.
if exist "%TMP%\x\merge_config.ps1" powershell -NoProfile -ExecutionPolicy Bypass -File "%TMP%\x\merge_config.ps1" >nul 2>nul

rem What is installed now: the game reads this for re-cut detection and flavour.
if not exist "JERICHO\CONFIG" mkdir "JERICHO\CONFIG"
> "JERICHO\CONFIG\installed.json" (
	echo {
	echo   "tag": "%TAG%",
	echo   "asset": "%ASSET%",
	echo   "sha256": "%GOT%"
	echo }
)

echo.
echo update_jericho: done.
echo   updated:  the game, JERICHO\ and its released modules
echo   kept:     CONFIG\  (your toggles and settings)
echo             config.ini  your window size and fullscreen (new keys added, yours kept)
echo             dediserver.json  your dedicated server's own settings
echo             DRIVER\  (your baked Driver 1 car data)
echo             any module you added under MODS\
echo.
echo Note: files are overwritten, never deleted, so an old file this release no
echo longer ships may still be here. That is what keeps your own mods safe.
echo Turn crumple and Caine's Crossfire on in Options - JERICHO if you want them.
rmdir /s /q "%TMP%" 2>nul
if defined RELAUNCH (
	echo Starting JERICHO again ...
	start "" "%RELAUNCH%"
	timeout /t 3 >nul
	exit /b 0
)
pause
exit /b 0

:fail
echo.
echo update_jericho: FAILED -- see above. Nothing was left half-applied.
pause
rmdir /s /q "%TMP%" 2>nul
if defined RELAUNCH start "" "%RELAUNCH%"
exit /b 1

rem Wait for the game (the button passes its PID) to exit, so its files can be
rem replaced. Gives up after about a minute and carries on: robocopy retries a
rem locked file once and reports it.
:waitgame
set /a WAITN=0
echo waiting for JERICHO (pid %WAITPID%) to close ...
:waitloop
tasklist /FI "PID eq %WAITPID%" 2>nul | find "%WAITPID%" >nul
if errorlevel 1 exit /b 0
set /a WAITN+=1
if %WAITN% geq 60 exit /b 0
timeout /t 1 /nobreak >nul
goto waitloop

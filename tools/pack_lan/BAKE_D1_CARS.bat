@echo off
rem ============================================================================
rem  BAKE_D1_CARS.bat -- build the Driver 1 CAR files JERICHO reads, from YOUR
rem                     own copy of Driver 1.
rem
rem  JERICHO does NOT ship Driver 1's content -- it is copyrighted. The seven retail
rem  Driver 1 car-data cities (MIAMI, FRISCO, LA, NEWYORK, NEWCASTLE, IVIEW, TRAIN)
rem  read files baked OUT of it, so this script makes them from data you already
rem  own. If you also have the Euro Demo 43, Beta Miami (BETAMIAMI) comes along.
rem
rem  WHAT TO DO
rem    1. Put your Driver 1 game data in DRIVER\, beside DRIVER2\ -- the same
rem       folder layout the game itself uses, so DRIVER\LEVELS\*.LEV is present.
rem       For Beta Miami, put the Euro Demo 43 LEVELS and SOUND folders in
rem       EURO43\ (its level is EURO43\LEVELS\MIAMI.LEV).
rem    2. Run this script once. It writes JERICHO\MODS\carhacks\D1CARS\.
rem    3. Play. carhacks offers the cities from then on.
rem
rem  The baked blobs live INSIDE carhacks: they are its modded level data, not
rem  part of your Driver 1 disc, and the game reads them from there. The folder is
rem  generated, gitignored, and never part of a release.
rem
rem  Re-run it after updating JERICHO if the game says D1CARS is out of date
rem  (bakes from before v0.10.9 lack the D1 wheels and sounds). Re-running is safe.
rem
rem  NEEDS: python on PATH; your Driver 2 data in DRIVER2\ (the wheel geometry
rem  and ride height are derived from Driver 2's own car files, not guessed).
rem ============================================================================
setlocal
cd /d "%~dp0"

set "T=%~dp0JERICHO\MODS\d1cars\tools"
rem The dev tree's game folder (src_rebuild\bin\<config>) does not carry the
rem data-only d1cars module, so fall back to the repo's copy three levels up.
rem A shipped package has the tools beside the script and takes the first path.
if not exist "%T%\bake.py" if exist "%~dp0..\..\..\JERICHO\MODS\d1cars\tools\bake.py" set "T=%~dp0..\..\..\JERICHO\MODS\d1cars\tools"
set "LEV=DRIVER\LEVELS"
set "D2=DRIVER2\LEVELS"
set "OUT=JERICHO\MODS\carhacks\D1CARS"
set "CITIES=MIAMI,FRISCO,LA,NEWYORK,NEWCASTLE,IVIEW,TRAIN"
set "BETA=0"

echo BAKE_D1_CARS: baking Driver 1 car data into %OUT%
echo.

if not exist "%T%\bake.py" (
	echo   FAILED: "%T%\bake.py" is missing.
	echo   This package was built without the d1cars tools, so there is nothing to
	echo   bake with. Re-download the package, or run the tools from the repo.
	goto :fail
)

if not exist "%LEV%" (
	echo   FAILED: no "%LEV%".
	echo   Put your Driver 1 game data in DRIVER\ first -- the layout the game
	echo   uses, so that DRIVER\LEVELS\*.LEV exists.
	goto :fail
)

if not exist "%D2%" (
	echo   FAILED: no "%D2%".
	echo   The bake measures its wheels and ride height against Driver 2's own
	echo   car files, so DRIVER2\ has to be the real game data too.
	goto :fail
)

set "PY=python"
%PY% --version >nul 2>nul
if errorlevel 1 set "PY=py -3"
%PY% --version >nul 2>nul
if errorlevel 1 (
	echo   FAILED: no python on PATH.
	echo   Install Python 3 from python.org ^(tick "Add python.exe to PATH"^), then
	echo   run this again.
	goto :fail
)

rem Euro Demo 43 is optional. Detect it once, and let the player see that we found
rem it: Beta Miami only exists in that leaked beta build.
if exist "EURO43\LEVELS\MIAMI.LEV" (
	set "BETA=1"
	echo   * Euro Demo 43 detected in EURO43\ -- Beta Miami is joining the garage.
	echo.
) else if exist "EURO43\LEVELS\BETAMIAMI.LEV" (
	set "BETA=1"
	echo   * Euro Demo 43 detected in EURO43\ ^(renamed BETAMIAMI.LEV^) -- Beta Miami is joining the garage.
	echo.
) else (
	echo   ^(no EURO43\LEVELS\MIAMI.LEV -- Beta Miami stays parked; baking the seven retail cities^)
	echo.
)
if "%BETA%"=="1" set "CITIES=%CITIES%,BETAMIAMI"

rem BAKE_VERSION: bump when the bake gains something the game reads. 8 moved the
rem output into carhacks and made the EURO-43 beta read the demo's MIAMI.LEV.
set "BAKE_VERSION=8"
set "HAVE_VERSION="
if exist "%OUT%\BAKE_VERSION" set /p HAVE_VERSION=<"%OUT%\BAKE_VERSION"
if exist "%OUT%" if not "%HAVE_VERSION%"=="%BAKE_VERSION%" (
	echo Your existing %OUT% is from an older bake ^(no D1 wheels/sounds, pre-Miami wheels, unpacked palette rows, or the old DRIVER\D1CARS location^) -- rebuilding it.
	echo.
)
if not exist "%OUT%" if exist "DRIVER\D1CARS" (
	echo Found your old bake in DRIVER\D1CARS -- rebuilding it inside carhacks,
	echo where the game now reads it.
	echo.
)

if not exist "%OUT%" mkdir "%OUT%"

rem The pool first: every distinct traffic shape in your Driver 1 levels.
echo [1/5] reading Driver 1's car shapes ...
%PY% "%T%\d1pool.py" --levels "%LEV%" || goto :fail

rem One blob per city -- each is a complete small "level" holding that city's cars,
rem their palette and their texture pages.
echo [2/5] baking one blob per city ...
for %%C in (MIAMI FRISCO LA NEWYORK NEWCASTLE IVIEW TRAIN) do (
	%PY% "%T%\bake.py" --pool "%T%\out\pool.json" --levels "%LEV%" --city %%C --out "%OUT%" || goto :fail
)
if "%BETA%"=="1" (
	%PY% "%T%\bake.py" --pool "%T%\out\pool.json" --levels "%LEV%" --city BETAMIAMI --out "%OUT%" || goto :fail
)

rem The cosmetics: a .LCF per city, carrying the per-car wheel placement. Driver 2's
rem own files are the donor, so these describe YOUR copy's proportions.
echo [3/5] writing the car cosmetics ...
%PY% "%T%\cosmetics1.py" --levels "%LEV%" --donor "%D2%" --cities "%CITIES%" --write "%OUT%" || goto :fail

rem The sounds: Driver 1's engine/idle/horn, from YOUR DRIVER\SOUND\VOICES.BLK,
rem patched into each freshly baked city. BETAMIAMI uses the Euro-43 beta's own
rem SOUND.SBK when it is present (pitched-down traffic idle + acceleration loop).
echo [4/5] adding the car sounds ...
if exist "DRIVER\SOUND\VOICES.BLK" (
	for %%C in (MIAMI FRISCO LA NEWYORK NEWCASTLE IVIEW TRAIN) do (
		%PY% "%T%\add_car_sounds.py" --lev "%OUT%\%%C.LEV" --voices "DRIVER\SOUND\VOICES.BLK" --city %%C || goto :fail
	)
) else (
	echo   ^(no DRIVER\SOUND\VOICES.BLK -- Driver 1 cars will use the host city's sounds^)
)

if "%BETA%"=="1" (
	if exist "EURO43\SOUND\SOUND.SBK" (
		%PY% "%T%\add_car_sounds.py" --lev "%OUT%\BETAMIAMI.LEV" --voices "EURO43\SOUND\SOUND.SBK" --city BETAMIAMI || goto :fail
	) else if exist "DRIVER\SOUND\VOICES.BLK" (
		%PY% "%T%\add_car_sounds.py" --lev "%OUT%\BETAMIAMI.LEV" --voices "DRIVER\SOUND\VOICES.BLK" --city BETAMIAMI || goto :fail
	) else (
		echo   ^(no EURO43\SOUND\SOUND.SBK or DRIVER\SOUND\VOICES.BLK -- BETAMIAMI will use the host city's sounds^)
	)
)

rem Beta Miami is a runtime BANK, not one of the seven compile-time rows. The engine
rem registers it automatically from the baked file; this cities.ini row is also
rem written for older builds and to pin the slot. Add it idempotently if we baked it.
if "%BETA%"=="1" (
	echo [5/5] registering Beta Miami with the city registry ...
	set "REG="
	if exist "%~dp0d1city_register.py" set "REG=%~dp0d1city_register.py"
	if not defined REG if exist "%~dp0tools\d1city_register.py" set "REG=%~dp0tools\d1city_register.py"
	if not defined REG if exist "%~dp0..\..\..\tools\d1city_register.py" set "REG=%~dp0..\..\..\tools\d1city_register.py"
	if defined REG if exist "%OUT%\BETAMIAMI.LEV" (
		%PY% "%REG%" --install "%~dp0." || goto :fail
	) else (
		echo   ^(no d1city_register.py -- Beta Miami is baked, but add its row to
		echo    JERICHO\CONFIG\cities.ini by hand to have the game offer it^)
	)
) else (
	echo [5/5] no Euro Demo 43 -- nothing to register for Beta Miami.
)

>"%OUT%\BAKE_VERSION" echo %BAKE_VERSION%

echo.
echo BAKE_D1_CARS: done. %OUT% now holds:
dir /b "%OUT%"
echo.
if "%BETA%"=="1" if exist "%OUT%\BETAMIAMI.LEV" (
	echo   Beta Miami is in. Go enjoy your favourite yellow Buick GX.
) else (
	echo   Start the game and the seven retail Driver 1 cities will be offered.
	echo   ^(Put the Euro Demo 43's LEVELS and SOUND in EURO43\ and re-run for Beta Miami.^)
)
pause
exit /b 0

:fail
echo.
echo BAKE_D1_CARS: FAILED -- nothing was changed in a way you cannot re-run.
pause
exit /b 1

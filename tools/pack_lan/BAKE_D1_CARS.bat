@echo off
rem ============================================================================
rem  BAKE_D1_CARS.bat -- build the Driver 1 CAR files JERICHO reads, from YOUR
rem                     own copy of Driver 1.
rem
rem  JERICHO does NOT ship Driver 1's content -- it is copyrighted. The five
rem  Driver 1 car-data cities (MIAMI, FRISCO, LA, NEWYORK, NEWCASTLE) read files
rem  baked OUT of it, so this script makes them from data you already own.
rem
rem  WHAT TO DO
rem    1. Put your Driver 1 game data in DRIVER\, beside DRIVER2\ -- the same
rem       folder layout the game itself uses, so DRIVER\LEVELS\*.LEV is present.
rem    2. Run this script once. It writes DRIVER\D1CARS\.
rem    3. Play. carhacks offers the five cities from then on.
rem
rem  You do not need this if DRIVER\D1CARS already exists. Re-running is safe.
rem
rem  NEEDS: python on PATH; your Driver 2 data in DRIVER2\ (the wheel geometry
rem  and ride height are derived from Driver 2's own car files, not guessed).
rem ============================================================================
setlocal
cd /d "%~dp0"

set "T=%~dp0JERICHO\MODS\d1cars\tools"
set "LEV=DRIVER\LEVELS"
set "D2=DRIVER2\LEVELS"
set "OUT=DRIVER\D1CARS"
set "CITIES=MIAMI,FRISCO,LA,NEWYORK,NEWCASTLE"

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

if not exist "%OUT%" mkdir "%OUT%"

rem The pool first: every distinct traffic shape in your Driver 1 levels.
echo [1/3] reading Driver 1's car shapes ...
%PY% "%T%\d1pool.py" --levels "%LEV%" || goto :fail

rem One blob per city -- each is a complete small "level" holding that city's cars,
rem their palette and their texture pages.
echo [2/3] baking one blob per city ...
for %%C in (MIAMI FRISCO LA NEWYORK NEWCASTLE) do (
	%PY% "%T%\bake.py" --pool "%T%\out\pool.json" --levels "%LEV%" --city %%C --out "%OUT%" || goto :fail
)

rem The cosmetics: a .LCF per city, carrying the per-car wheel placement. Driver 2's
rem own files are the donor, so these describe YOUR copy's proportions.
echo [3/3] writing the car cosmetics ...
%PY% "%T%\cosmetics1.py" --levels "%LEV%" --donor "%D2%" --cities "%CITIES%" --write "%OUT%" || goto :fail

echo.
echo BAKE_D1_CARS: done. %OUT% now holds:
dir /b "%OUT%"
echo.
echo Start the game and the five Driver 1 cities will be offered.
pause
exit /b 0

:fail
echo.
echo BAKE_D1_CARS: FAILED -- nothing was changed in a way you cannot re-run.
pause
exit /b 1

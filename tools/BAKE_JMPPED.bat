@echo off
rem ============================================================
rem  BAKE_JMPPED - thin Windows shim over jmpped_bake.py
rem
rem  Extracts Driver 2's Tanner and Jericho peds, exports a per-ped
rem  OBJ/PNG/skeleton workspace and rebuilds JMPPED v3.
rem
rem  Steps:  export | split | build | preview | all
rem  Examples:
rem    BAKE_JMPPED.bat all
rem    BAKE_JMPPED.bat export --source DRIVER2\LEVELS\HAVANA.LEV
rem    BAKE_JMPPED.bat split --and-build
rem    BAKE_JMPPED.bat build --copy
rem ============================================================
setlocal EnableExtensions

set "HERE=%~dp0"
set "PY="

where py >nul 2>nul
if not errorlevel 1 set "PY=py -3"

if not defined PY (
    where python >nul 2>nul
    if not errorlevel 1 set "PY=python"
)

if not defined PY (
    echo JMPPED: Python 3 was not found - install it and put it on PATH.
    exit /b 3
)

%PY% "%HERE%jmpped_bake.py" %*
exit /b %errorlevel%

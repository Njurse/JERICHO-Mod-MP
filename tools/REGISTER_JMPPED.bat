@echo off
rem Drag a completed JMPPED entry folder onto this file.
rem The folder must contain ped.json, obj\raw|edit and png\.
setlocal
if "%~1"=="" (
  echo Drag a completed JMPPED entry folder onto this file.
  echo It must contain ped.json, obj\ and png\.
  pause
  exit /b 1
)
python "%~dp0jmpped_register.py" --folder "%~1"
set RC=%ERRORLEVEL%
if not "%RC%"=="0" pause
exit /b %RC%

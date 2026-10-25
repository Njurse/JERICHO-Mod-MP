@echo off
rem Console wizard for the JMPPED toolkit.
setlocal
python "%~dp0jmpped_wizard.py" %*
if errorlevel 1 pause

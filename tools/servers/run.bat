@echo off
rem ============================================================================
rem run.bat -- one way in to the server-side tooling, for Windows.
rem            run.sh is the same thing for Linux/macOS, and is the documented one.
rem
rem   run.bat master             the DISCOVERY SERVER: the registry players list from
rem   run.bat dedi [options]     a headless DEDICATED SERVER, registered with it
rem   run.bat stack [options]    master + dedi + N real clients on this one PC (the rig)
rem   run.bat checks             the three checks CI runs for this folder
rem   run.bat docker             docker compose up -d --build, for a VPS
rem   run.bat help
rem
rem   Everything after the verb goes to the tool itself, so the full option lists are
rem   in masterserver\README.md, dediserver\README.md and TESTING.md.
rem
rem   MP_PYTHON names the interpreter when `python` is not the one you want.
rem ============================================================================
setlocal
set "HERE=%~dp0"
if "%MP_PYTHON%"=="" set "MP_PYTHON=python"
set "VERB=%~1"

if "%VERB%"=="" goto :help
if /i "%VERB%"=="help" goto :help
if /i "%VERB%"=="--help" goto :help
if /i "%VERB%"=="-h" goto :help

shift
if /i "%VERB%"=="master" (
    echo run: masterserver ^(the discovery registry^)
    "%MP_PYTHON%" "%HERE%masterserver\masterserver.py" %*
    exit /b %ERRORLEVEL%
)
if /i "%VERB%"=="dedi" (
    echo run: dediserver ^(a headless dedicated server^)
    "%MP_PYTHON%" "%HERE%dediserver\dediserver.py" %*
    exit /b %ERRORLEVEL%
)
if /i "%VERB%"=="stack" (
    echo run: the local stack rig ^(master + dedi + real clients on this PC^)
    "%MP_PYTHON%" "%HERE%rigs\local_stack.py" %*
    exit /b %ERRORLEVEL%
)
if /i "%VERB%"=="checks" (
    set "FAIL=0"
    for %%T in (proto_check.py masterserver\test_masterserver.py rigs\test_e2e.py) do (
        echo.
        echo == %%T
        "%MP_PYTHON%" "%HERE%%%T" || set "FAIL=1"
    )
    echo.
    if "%FAIL%"=="1" (
        echo run: checks FAILED ^(see above^)
    ) else (
        echo run: checks OK -- wire mirror, the master, and master+dedi+client end to end
    )
    exit /b %FAIL%
)
if /i "%VERB%"=="docker" (
    where docker >nul 2>&1 || (
        echo run: no docker on PATH
        exit /b 1
    )
    docker compose -f "%HERE%docker-compose.yml" up -d --build
    exit /b %ERRORLEVEL%
)

echo run: unknown verb "%VERB%" ^(try: master, dedi, stack, checks, docker, help^)
exit /b 2

:help
echo   run.bat master             the DISCOVERY SERVER: the registry players list from
echo   run.bat dedi [options]     a headless DEDICATED SERVER, registered with it
echo   run.bat stack [options]    master + dedi + N real clients on this one PC
echo   run.bat checks             the three checks CI runs for this folder
echo   run.bat docker             docker compose up -d --build, for a VPS
echo   run.bat help
echo.
echo   The same verbs exist as ./run.sh on Linux/macOS.
endlocal
exit /b 0

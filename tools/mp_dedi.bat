@echo off
rem ============================================================================
rem mp_dedi.bat [options]
rem
rem   A headless DEDICATED SERVER: hosts a real session with no game window, so a
rem   client has something to join, and so a server can be left up while the client
rem   side restarts. Ports and the beacon behave like a hosted game's.
rem
rem   It is pure Python and needs NO game build on this machine -- the exe check the
rem   old wrapper did is gone, because a dedicated server never loads the game.
rem
rem       mp_dedi.bat                    host on the default port (1318)
rem       mp_dedi.bat --port 1400        ... on another one
rem       mp_dedi.bat --name "Bob's 24/7" --no-beacon --idle-drop 0
rem       mp_dedi.bat --master http://master.example.com:8100
rem
rem   Config lives in servers\dediserver\dediserver.json (CLI flags override it).
rem   Ctrl-C stops it. It logs joins and leaves, which is the quickest way to tell a
rem   client that never connected from one that connected and was dropped.
rem
rem   Ports to open and the full option list: servers\dediserver\README.md
rem ============================================================================
setlocal
python "%~dp0servers\dediserver\dediserver.py" %*
set RC=%ERRORLEVEL%
endlocal
exit /b %RC%

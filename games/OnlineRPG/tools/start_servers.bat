@echo off
rem Starts the back end of OnlineRPG on this PC, each program in its own console window (close a
rem window or press Ctrl+C in it to stop that server):
rem   oe_login    7001   accounts, session tickets
rem   oe_channel  7002   players choosing a channel;  7003  the game servers' link
rem   oe serve-game  7778 = Channel 1, 7779 = Channel 2  (the worlds)
rem Accounts and keys live in build\server-data. Then start players with
rem   build\bin\oe.exe run games/OnlineRPG        and click Login.
setlocal
set ROOT=%~dp0..\..\..
set BIN=%ROOT%\build\bin
set DATA=%ROOT%\build\server-data
if not exist "%BIN%\oe.exe" (
    echo %BIN%\oe.exe not found. Run build.bat in %ROOT% first.
    exit /b 1
)
if not exist "%BIN%\oe_channel.exe" (
    echo %BIN%\oe_channel.exe not found. Run games\OnlineRPG\server\build.bat first.
    exit /b 1
)
if not exist "%DATA%" mkdir "%DATA%"

start "OnlineRPG login 7001" "%BIN%\oe_login.exe" --port 7001 --db "%DATA%\accounts.db" --key-file "%DATA%\server.key"
start "OnlineRPG channel server 7002/7003" "%BIN%\oe_channel.exe" --port 7002 --game-port 7003 --key-file "%DATA%\server.key" --game-key-file "%DATA%\game.key"

rem The channel server writes game.key on its first start; the game servers read it when they start.
set TRIES=0
:wait
if exist "%DATA%\game.key" goto ready
set /a TRIES+=1
if %TRIES% gtr 50 (
    echo The channel server did not write %DATA%\game.key. Look at its window.
    exit /b 1
)
ping -n 1 -w 200 192.0.2.1 >nul
goto wait
:ready

start "OnlineRPG Channel 1 (7778)" "%BIN%\oe.exe" serve-game "%~dp0.." --port 7778 --set channel=1 --set map=town --set platform=127.0.0.1:7003 --set-file "linkkey=%DATA%\game.key"
start "OnlineRPG Channel 2 (7779)" "%BIN%\oe.exe" serve-game "%~dp0.." --port 7779 --set channel=2 --set map=town --set platform=127.0.0.1:7003 --set-file "linkkey=%DATA%\game.key"
echo Started the login server, the channel server and two game servers.

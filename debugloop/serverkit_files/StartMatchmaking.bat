@echo off
rem Starts the matchmaking service. It lists games in the players' server browser and keeps
rem the game servers in matchmaking\appsettings.json ("Roster") running; it starts them itself,
rem so do not also run StartServer.bat. Leave its window open. StopMatchmaking.bat stops everything.
cd /d "%~dp0"
if not exist "%~dp0matchmaking\gamecontroller.exe" (
    echo The matchmaking folder is missing. Unzip a server kit made with the service into this folder.
    pause
    exit /b 1
)
tasklist /fi "imagename eq gamecontroller.exe" | find /i "gamecontroller.exe" >nul
if not errorlevel 1 (
    echo The matchmaking service is already running.
    pause
    exit /b 0
)
if not exist "%~dp0Serverborn.exe" copy /y "%~dp0Battleborn.exe" "%~dp0Serverborn.exe" >nul
echo Starting the matchmaking service on port 5000. Game servers appear within a minute or two.
echo Players: choose Story or Versus in the game; the server browser lists this machine's games.
start "Battleborn Reborn matchmaking" /min "%~dp0matchmaking\gamecontroller.exe"

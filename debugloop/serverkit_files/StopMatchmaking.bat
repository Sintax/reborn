@echo off
rem Stops the matchmaking service and every game server it started. Players are disconnected.
taskkill /im gamecontroller.exe /f >nul 2>&1
if errorlevel 1 (echo The matchmaking service was not running.) else (echo Matchmaking service stopped.)
taskkill /im Serverborn.exe /f >nul 2>&1
if errorlevel 1 (echo No game server was running.) else (echo Game servers stopped.)
pause

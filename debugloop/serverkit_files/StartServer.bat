@echo off
rem Starts the Battleborn Reborn server. Extra words go to StartServer.ps1, e.g.
rem   StartServer.bat -Map Portal_P -Players 2
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0StartServer.ps1" %*
pause

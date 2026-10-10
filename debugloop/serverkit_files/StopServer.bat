@echo off
rem Stops the Battleborn Reborn server. Players in the game are disconnected.
tasklist /fi "imagename eq Serverborn.exe" | find /i "Serverborn.exe" >nul
if errorlevel 1 (
    echo No server is running.
) else (
    taskkill /im Serverborn.exe /f >nul
    echo Server stopped.
)
pause

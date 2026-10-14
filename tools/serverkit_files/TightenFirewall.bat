@echo off
rem Narrows the Battleborn firewall rules to exactly what players need:
rem   the game servers (Serverborn.exe) on UDP 7777-7779
rem   the server browser (matchmaking\gamecontroller.exe) on TCP 5000
rem Removes the wide "any port" rules Windows made when the programs first ran,
rem and turns off the unused "Allow 7777" TCP rule. Saves a backup first.
rem Put this file in the game's Win64 folder (next to Serverborn.exe) and run it.
rem Windows asks for administrator rights; say yes.
setlocal
net session >nul 2>&1
if errorlevel 1 (
    powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'"
    exit /b
)

set "WIN64=%~dp0"
if not exist "%WIN64%Serverborn.exe" set "WIN64=D:\battleborn\binaries\win64\"
set "SERVER=%WIN64%Serverborn.exe"
set "MATCH=%WIN64%matchmaking\gamecontroller.exe"
if not exist "%SERVER%" (
    echo Cannot find Serverborn.exe. Put this file in the game's Win64 folder and run it again.
    echo Nothing was changed.
    pause
    exit /b 1
)
if not exist "%MATCH%" (
    echo Cannot find %MATCH%. Nothing was changed.
    pause
    exit /b 1
)

for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "STAMP=%%t"
set "BACKUP=%WIN64%firewall-backup-%STAMP%.wfw"
netsh advfirewall export "%BACKUP%" >nul
if errorlevel 1 (
    echo Could not save a backup of the firewall settings. Nothing was changed.
    pause
    exit /b 1
)
echo Backup saved: %BACKUP%

rem Wide rules: any port for these programs, or any program on these ports.
netsh advfirewall firewall delete rule name="Battleborn" >nul 2>&1
netsh advfirewall firewall delete rule name="gamecontroller" >nul 2>&1
netsh advfirewall firewall delete rule name="Battleborn Reborn server" >nul 2>&1
netsh advfirewall firewall delete rule name="Battleborn Reborn matchmaking" >nul 2>&1
netsh advfirewall firewall set rule name="Allow 7777" new enable=no >nul 2>&1

set FAILED=0
netsh advfirewall firewall add rule name="Battleborn Reborn server" dir=in action=allow profile=any ^
    program="%SERVER%" protocol=UDP localport=7777-7779 edge=no ^
    description="Players joining the game servers. Only Serverborn.exe, only UDP 7777-7779." >nul
if errorlevel 1 set FAILED=1
netsh advfirewall firewall add rule name="Battleborn Reborn matchmaking" dir=in action=allow profile=any ^
    program="%MATCH%" protocol=TCP localport=5000 edge=no ^
    description="The in-game server browser. Only gamecontroller.exe, only TCP 5000." >nul
if errorlevel 1 set FAILED=1

if %FAILED%==1 (
    echo Something went wrong; putting the old settings back.
    netsh advfirewall import "%BACKUP%" >nul
    echo Old settings restored. Nothing changed.
    pause
    exit /b 1
)

echo.
echo Done. The firewall now lets in only:
echo   Serverborn.exe on UDP 7777-7779  (players in a match)
echo   gamecontroller.exe on TCP 5000   (the server browser)
echo The "Allow 7777" TCP rule is turned off, not deleted.
echo.
echo If anything stops working, undo everything with:
echo   netsh advfirewall import "%BACKUP%"
echo (run it in an administrator Command Prompt)
pause

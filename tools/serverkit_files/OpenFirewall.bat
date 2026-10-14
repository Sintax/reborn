@echo off
rem Lets players reach the servers (Serverborn.exe, UDP 7777-7779) and the matchmaking
rem service (matchmaking\gamecontroller.exe, TCP 5000) through Windows Firewall. Run once.
rem Windows asks for administrator rights; say yes.
net session >nul 2>&1
if errorlevel 1 (
    powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'"
    exit /b
)
netsh advfirewall firewall delete rule name="Battleborn Reborn server" >nul 2>&1
netsh advfirewall firewall delete rule name="Battleborn Reborn matchmaking" >nul 2>&1
netsh advfirewall firewall add rule name="Battleborn Reborn server" dir=in action=allow profile=any program="%~dp0Serverborn.exe" protocol=UDP localport=7777-7779 edge=no
if errorlevel 1 (
    echo Could not add the firewall rule for the game servers.
) else (
    echo Done: Windows Firewall now lets players reach Serverborn.exe on UDP ports 7777-7779.
)
netsh advfirewall firewall add rule name="Battleborn Reborn matchmaking" dir=in action=allow profile=any program="%~dp0matchmaking\gamecontroller.exe" protocol=TCP localport=5000 edge=no
if errorlevel 1 (
    echo Could not add the firewall rule for the matchmaking service.
) else (
    echo Done: Windows Firewall now lets the game's server browser reach gamecontroller.exe on TCP port 5000.
)
echo If your hosting company has its own firewall, it must allow UDP 7777-7779 and TCP 5000 too.
pause

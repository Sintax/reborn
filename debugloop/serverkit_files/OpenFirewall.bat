@echo off
rem Lets players reach the servers (UDP 7777-7779) and the matchmaking service (TCP 5000)
rem through Windows Firewall. Run once.
rem Windows asks for administrator rights; say yes.
net session >nul 2>&1
if errorlevel 1 (
    powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'"
    exit /b
)
netsh advfirewall firewall delete rule name="Battleborn Reborn server" >nul 2>&1
netsh advfirewall firewall delete rule name="Battleborn Reborn matchmaking" >nul 2>&1
netsh advfirewall firewall add rule name="Battleborn Reborn server" dir=in action=allow protocol=UDP localport=7777-7779
if errorlevel 1 (
    echo Could not add the firewall rule for the game servers.
) else (
    echo Done: Windows Firewall now lets players in on UDP ports 7777-7779.
)
netsh advfirewall firewall add rule name="Battleborn Reborn matchmaking" dir=in action=allow protocol=TCP localport=5000
if errorlevel 1 (
    echo Could not add the firewall rule for the matchmaking service.
) else (
    echo Done: Windows Firewall now lets the game's server browser in on TCP port 5000.
)
echo Your hosting company's firewall must allow UDP 7777-7779 and TCP 5000 too; set that on their website.
pause

@echo off
rem Lets players reach the server through Windows Firewall (UDP port 7777). Run once.
rem Windows asks for administrator rights; say yes.
net session >nul 2>&1
if errorlevel 1 (
    powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'"
    exit /b
)
netsh advfirewall firewall delete rule name="Battleborn Reborn server" >nul 2>&1
netsh advfirewall firewall add rule name="Battleborn Reborn server" dir=in action=allow protocol=UDP localport=7777
if errorlevel 1 (
    echo Could not add the firewall rule.
) else (
    echo Done: Windows Firewall now lets players in on UDP port 7777.
    echo Your hosting company's firewall must allow UDP 7777 too; set that on their website.
)
pause

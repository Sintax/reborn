# Starts the Battleborn Reborn server on this machine. StartServer.bat runs this.
#
#   StartServer.bat                              asks which mission and how many players
#   StartServer.bat -Map Portal_P -Players 2     no questions
#   StartServer.bat -DryRun                      prepares and checks everything, starts nothing
#
# Players join with Direct Connect to this machine's public address, UDP port 7777.
# Written for Windows PowerShell 5.1, which every Windows 10/11 and Server machine has.
param(
    [string]$Map = "",
    [int]$Players = 0,
    [switch]$DryRun
)
$ErrorActionPreference = "Stop"

$KitVersion = "{{VERSION}}"
$Win64 = $PSScriptRoot
$Name = "reborn-server"
$SteamId = "{{STEAMID}}"
$Port = 7777
$Missions = @(
    @("Caverns_P", "The Algorithm (tested the most)"),
    @("PvE_Prologue_P", "Prologue"),
    @("Portal_P", "Void's Edge"),
    @("Captains_P", "The Renegade"),
    @("Evacuation_P", "The Archive"),
    @("Ruins_P", "Sentinel"),
    @("Observatory_p", "The Experiment"),
    @("Refinery_P", "The Saboteur"),
    @("Cathedral_P", "Heliophage")
)
$LoaderFiles = @("steamclient_loader_x64.exe", "steamclient.dll", "steamclient64.dll")

function Fail([string]$msg) {
    Write-Host ""
    Write-Host "PROBLEM: $msg" -ForegroundColor Red
    exit 1
}

function Write-NoBom([string]$path, [string]$text) {
    # The loader reads its ini as plain text; a UTF-8 byte-order mark would spoil the first line.
    [IO.File]::WriteAllText($path, $text, (New-Object Text.UTF8Encoding $false))
}

function Same-File([string]$a, [string]$b) {
    if (-not (Test-Path $a) -or -not (Test-Path $b)) { return $false }
    return (Get-FileHash $a).Hash -eq (Get-FileHash $b).Hash
}

Write-Host "Battleborn Reborn server kit $KitVersion"

# --- Is everything here? ---------------------------------------------------------------------
if (-not (Test-Path "$Win64\Battleborn.exe")) {
    Fail "Battleborn.exe is not in $Win64. Unzip the server kit into the game's Binaries\Win64 folder and run it from there."
}
foreach ($f in @("dxgi.dll", "reborn.dll") + $LoaderFiles) {
    if (-not (Test-Path "$Win64\$f")) {
        Fail "$f is missing from $Win64. If antivirus removed it, restore or allow it in Windows Security, then unzip the kit again."
    }
}
if (Get-Process -Name Serverborn -ErrorAction SilentlyContinue) {
    Fail "A server is already running. Run StopServer.bat first if you want to start a new one."
}

# --- Which mission, how many players ----------------------------------------------------------
if (-not $Map) {
    Write-Host ""
    for ($i = 0; $i -lt $Missions.Count; $i++) {
        Write-Host ("  {0}. {1}" -f ($i + 1), $Missions[$i][1])
    }
    $pick = Read-Host "Which mission? (Enter for 1)"
    if (-not $pick) { $pick = "1" }
    $n = 0
    if (-not [int]::TryParse($pick, [ref]$n) -or $n -lt 1 -or $n -gt $Missions.Count) {
        Fail "'$pick' is not one of the numbers above."
    }
    $Map = $Missions[$n - 1][0]
}
$known = $Missions | Where-Object { $_[0] -eq $Map }
if (-not $known) { Write-Host "Note: $Map is not in the mission list; trying it anyway." -ForegroundColor Yellow }

if ($Players -eq 0) {
    $pick = Read-Host "How many players will join? (1 to 5, Enter for 1)"
    if (-not $pick) { $pick = "1" }
    if (-not [int]::TryParse($pick, [ref]$Players)) { Fail "'$pick' is not a number." }
}
if ($Players -lt 1 -or $Players -gt 5) { Fail "Players must be 1 to 5, not $Players." }

# --- The server is a renamed copy of the game: the mod acts as the server when it runs as Serverborn.exe
$exe = "$Win64\Serverborn.exe"
if (-not (Same-File "$Win64\Battleborn.exe" $exe)) {
    Copy-Item "$Win64\Battleborn.exe" $exe -Force
    Write-Host "Made Serverborn.exe from Battleborn.exe"
}

# --- Its own Steam identity, so the loader's settings never clash with a player's on this machine
$id = "$Win64\rb_ids\$Name"
$cmdLine = "-windowed -nosound -nomoviestartup -NOSPLASH -nullrhi -rbservermap=$Map -rbplayers=$Players -rbinstance=$Name"
New-Item -ItemType Directory -Force "$id\steam_settings" | Out-Null
foreach ($f in $LoaderFiles) {
    if (-not (Same-File "$Win64\$f" "$id\$f")) { Copy-Item "$Win64\$f" "$id\$f" -Force }
}
if (Test-Path "$Win64\steam_settings") { Copy-Item "$Win64\steam_settings\*" "$id\steam_settings" -Recurse -Force }

Write-NoBom "$id\ColdClientLoader.ini" (@(
    "[SteamClient]",
    "Exe=$exe",
    "ExeRunDir=$Win64",
    "ExeCommandLine=$cmdLine",
    "AppId=394230",
    "SteamClientDll=$id\steamclient.dll",
    "SteamClient64Dll=$id\steamclient64.dll",
    "[Injection]",
    "ForceInjectSteamClient=0",
    "ForceInjectGameOverlayRenderer=0",
    "DllsToInjectFolder=",
    "IgnoreInjectionError=1",
    "IgnoreLoaderArchDifference=0",
    "[Persistence]",
    "Mode=0",
    "[Debug]",
    "ResumeByDebugger=0",
    "") -join "`r`n")

Write-NoBom "$id\steam_settings\configs.user.ini" (@(
    "[user::general]",
    "account_name=$Name",
    "account_steamid=$SteamId",
    "language=english",
    "[user::saves]",
    "local_save_path=$id\saves",
    "") -join "`r`n")

if ($DryRun) {
    Write-Host ""
    Write-Host "Dry run: everything is in place. Would start:"
    Write-Host "  Serverborn.exe $cmdLine"
    exit 0
}

# --- Start it and wait for the game to appear -------------------------------------------------
Start-Process -FilePath "$id\steamclient_loader_x64.exe" -WorkingDirectory $id | Out-Null
$deadline = (Get-Date).AddSeconds(60)
$server = $null
while (-not $server -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 500
    $server = Get-Process -Name Serverborn -ErrorAction SilentlyContinue | Select-Object -First 1
}
if (-not $server) {
    Fail "Serverborn.exe did not start within 60 seconds. If Steam is open on this machine, close it and try again. Logs: $env:USERPROFILE\Documents\RebornLogs"
}

$label = if ($known) { $known[1] } else { $Map }
Write-Host ""
Write-Host "Server started (process $($server.Id)) on $label, waiting for $Players player(s) on UDP port $Port." -ForegroundColor Green
Write-Host "Players: Direct Connect to this machine's public address (the one you remote into)."
Write-Host "Leave the server's black console window open. To stop it, run StopServer.bat."
Write-Host "Logs: $env:USERPROFILE\Documents\RebornLogs"

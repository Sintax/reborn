# Battleborn Reborn: running the server on a rented machine

The server is a copy of the game that runs a mission without drawing anything. Players on
their own PCs connect to it over the internet. A rented Windows machine works well for this:
it is always on, and it has its own public address, so nobody has to change router settings.

It needs about 3.5 GB of memory (Windows plus the server) and about 50 GB of disk. It does
not need a graphics card.

## One-time setup

1. **Disk space.** Make sure the drive you install to has at least 50 GB free.
2. **Install Battleborn with Steam** on that machine. Sign in to Steam, find Battleborn in
   your library, and install it. When it is done, **close Steam completely** (right-click
   the Steam icon by the clock, then Exit). The server uses its own small loader, and a
   running Steam can get in its way.
3. **Copy the server kit** (`Reborn-ServerKit-<version>.zip`) to the machine.
4. **Unzip it into the game's `Binaries\Win64` folder.** In Steam that is: right-click
   Battleborn, Manage, Browse local files, then `Binaries`, then `Win64`. You should see
   `Battleborn.exe` there. Say yes to replacing files.
5. If Windows Defender removes `steamclient_loader_x64.exe`, allow it in Windows Security.
   Loaders like this are often flagged even though they are harmless.
6. **Double-click `OpenFirewall.bat`** and say yes when Windows asks for administrator
   rights. This lets players through Windows Firewall.
7. **Open the hosting company's firewall too.** On the company's website (for example the
   Infosaic control panel), allow incoming **UDP ports 7777 to 7779** and **TCP port 5000**.
   This step can't be done from inside the machine.

## Matchmaking: games players can see and join from the menu

With the matchmaking service running, players do not type an address. In the game they
choose Story or Versus, the server browser lists this machine's games with player counts,
and they click **Join**.

1. Double-click **`StartMatchmaking.bat`**. A small window opens; **leave it open**. Within a
   minute or two it starts one game server per entry in its roster (a console window each).
2. Which games it runs is in `matchmakingppsettings.json` under `"Roster"`: a name, the
   map, and how many players the match waits for. The kit ships with one entry, The
   Algorithm for 2 players. Edit it with Notepad, then run `StopMatchmaking.bat` and
   `StartMatchmaking.bat` again. Each entry needs about 1.3 GB of memory; on a 6 GB machine
   keep it to one or two.
3. When a match ends, the service starts a fresh server for that entry by itself.
4. To stop everything, run **`StopMatchmaking.bat`**.

Do not run `StartServer.bat` while the service is running: both would want port 7777.
The service writes what it is doing to its window; the game servers log as usual in
`Documents\RebornLogs` (folders starting with `rb-`).

## Start a game by hand (no matchmaking)

1. Double-click **`StartServer.bat`**.
2. Pick a mission and say how many players will join. For testing on your own, say 1.
3. A black console window opens. That is the server. **Leave it open.**
4. On your own PC, start the game as usual, choose Story, click **Direct Connect**, and
   type the rented machine's public address (the same one you remote into).
5. When the mission is over, run **`StopServer.bat`**, then `StartServer.bat` again for
   the next game.

If a Windows Firewall pop-up appears the first time the server starts, click **Allow**.
Clicking Cancel can make Windows block the server, even with the port open.

**Leaving the remote desktop:** close the remote desktop window (or choose Disconnect).
Do **not** sign out. Signing out closes the server.

## A new build of the mod

Copy the new server kit over and unzip it into `Win64` again, replacing files. Stop the
server first, or Windows won't let the files be replaced.

## When something goes wrong

The server's logs are in `Documents\RebornLogs`, in a folder that starts with
`reborn-server-`. `reborn-server.log` is everything the server printed. If it crashed, the
same folder has a crash dump (`.dmp`) and a short crash report (`.json`).
`CollectLogs.bat` zips the newest folder onto the Desktop.

## For Claude on the server machine

You are setting up a Battleborn Reborn dedicated server on this machine. The owner remotes
in; nobody plays the game on this machine. Steps, with checks:

1. Find the game folder: the `Binaries\Win64` folder that holds `Battleborn.exe` (often
   `C:\Program Files (x86)\Steam\steamapps\common\Battleborn` or a `SteamLibrary` folder on
   another drive). Check the drive has room; the full install is about 47 GB.
2. Make sure `steam.exe` is not running (`Get-Process steam`). If it is, ask the owner
   before closing it.
3. Unzip the kit into that `Win64` folder, overwriting files. Confirm `dxgi.dll`,
   `reborn.dll` and `steamclient_loader_x64.exe` are present afterwards; Defender
   sometimes removes the loader.
4. Run `OpenFirewall.bat` (it needs administrator rights). Then confirm the rules exist with
   `Get-NetFirewallRule -DisplayName "Battleborn Reborn server"` and
   `Get-NetFirewallRule -DisplayName "Battleborn Reborn matchmaking"`.
5. Run a check that starts nothing:
   `powershell -NoProfile -ExecutionPolicy Bypass -File .\StartServer.ps1 -Map Caverns_P -Players 1 -DryRun`.
   It should end with "Dry run: everything is in place".
6. Start it for real: `.\StartServer.bat -Map Caverns_P -Players 1`.
7. Within a minute the newest `Documents\RebornLogs\reborn-server-*\reborn-server.log` should contain
   `Game networking listening on port 7777`, and
   `Get-NetUDPEndpoint -LocalPort 7777` should show the server listening.
8. Remind the owner that the hosting company's firewall must allow **UDP 7777-7779** and
   **TCP 5000**; that step is on their website, not on this machine.
9. Matchmaking, if the kit has a `matchmaking` folder: stop any hand-started server
   (`StopServer.bat`), then run `StartMatchmaking.bat`. Within two minutes
   `Invoke-RestMethod http://localhost:5000/api/games` should list one game with
   `serverConnectString` = `open <public ip>:7777`, and a `Serverborn.exe` process should be
   running with `-rbcoordinator=localhost:5000` on its command line
   (`Get-CimInstance Win32_Process -Filter "name='Serverborn.exe'" | Select CommandLine`).
   The service's own output is in its window; it says "Public address: ..." once it knows it.

If the server exits early, read the newest `reborn-server.log` and any `.json` crash report in
that folder, and report what they say. Do not change the game's files beyond unzipping the
kit.

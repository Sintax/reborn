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
7. **Open the hosting company's firewall too.** On the company's website (its control
   panel), allow incoming **UDP ports 7777 to 7779** and **TCP port 5000**.
   This step can't be done from inside the machine.

## Matchmaking: games players can see and join from the menu

With the matchmaking service running, players do not type an address. In the game they
choose Story or Versus, the server browser lists this machine's games with player counts,
and they click **Join**.

1. Double-click **`StartMatchmaking.bat`**. A small window opens; **leave it open**. Within a
   minute or two it starts one game server per entry in its roster (a console window each).
2. Which games it runs is in `matchmakingppsettings.json` under `"Roster"`: a name, the
   map, and how many players the match waits for. The kit ships with one entry, The
   Algorithm for 2 players. Two optional settings per entry: `"RandomHero": true` means the
   timer picks a hero for every player (nobody picks by hand; the setting reaches the game
   server through its config, not the command line). `"DraftStyle"` is one of `QuickSelect`
   (the default), `BanPick`, `CaptainsDraft`, `ChaosRumble` or `Classic`; it only applies to
   Versus, and the service turns it into the game's own value (Ban Pick and Captain's Draft use
   the 3v3 version when teams have 3 or fewer). Edit it with Notepad, then run `StopMatchmaking.bat` and
   `StartMatchmaking.bat` again. Each entry needs about 1.3 GB of memory; on a 6 GB machine
   keep it to one or two.
3. When a match ends, the service starts a fresh server for that entry by itself.
4. To stop everything, run **`StopMatchmaking.bat`**.

Do not run `StartServer.bat` while the service is running: both would want port 7777.
The service writes what it is doing to its window; the game servers log as usual in
`Documents\RebornLogs` (folders starting with `rb-`).

## Letting friends start games on the server

Players with a key can press **Start a Game** in the server browser, leave **Run on** as the
server, pick a map, mode, player count, a game name and (for Versus) bots per team and a draft
style, and the service starts that game for them. Games
started this way shut down when the match ends, when nobody joins within 3 minutes, or 1 minute
after the last player leaves. At most three games run at once (the three ports), counting the
roster.

Who may start games is a setting on each person's entry in the user list, `reborn-users.json`
in the game's `Binaries\Win64` folder. Everyone on the list needs a key to join. Only people
marked "create" can start games. Changes apply within a second, with no restart, and games
already running carry on.

1. On your own PC: `python -m tools.vm key <friend>` prints a key and writes
   `dist\keys\reborn-key-<friend>.txt`.
2. On this machine: `python -m tools.vm user add <friend> --key <key> --create` adds them and
   lets them start games. Leave out `--create` if they may only join. Leave out `--key` and the
   tool makes a key and prints it once.
3. Send the friend the file. They save it as `reborn-key.txt` in the game's `Binaries\Win64` folder.
4. To let someone start games, or stop them: `python -m tools.vm user create <friend> on` (or `off`).
   To cut someone off: `python -m tools.vm user revoke <friend>`.
   `python -m tools.vm user list` shows everyone, never their keys. `matchmaking.log` shows
   `Reloaded the user list: <count> users (<names>)` after each change. If the file is saved with
   a JSON mistake, the log warns and the last good list stays in use.

A player the server refuses sees a **Reborn** notice saying **Improper Credentials!**. That happens
with no key file, a key that is not on your list, a key that belongs to a different name, or after
you removed them. The player's fix: make sure the name in `reborn-player.txt` matches the name you
registered and `reborn-key.txt` is the file you sent. Otherwise add them again with a new key.

To change a name: `python -m tools.vm user rename <old> <new>`. The player must then use the new
name in `reborn-player.txt`.

"Require keys to join" is on by default. `python -m tools.vm user require-keys off` lets anyone join
(on again with `on`). `python -m tools.vm --help` lists every command.

**Upgrading from the old setup:** the first time the service starts with old `"StartKeys"` in its
settings and no user list, it turns them into users who can start games. After that, `StartKeys` is
no longer used. Those users work with their key alone until you rename them to the player's real
name with `user rename <old> <new>`; after that the player's `reborn-player.txt` must match.

The timeouts are `"RequestedGame": { "NoJoinSeconds": 180, "EmptySeconds": 60 }` in the matchmaking
`appsettings.json`. `matchmaking.log` records which player started each game.

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

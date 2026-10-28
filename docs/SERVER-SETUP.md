# Battleborn Reborn: running the server on a rented machine

The server is a copy of the game that runs a mission without drawing anything. Players on
their own PCs connect to it over the internet. A rented Windows machine works well for this:
it is always on, and it has its own public address, so nobody has to change router settings.

It needs about 3.5 GB of memory (Windows plus the server) and about 50 GB of disk. It does
not need a graphics card.

## One-time setup

The easy way is the server installer, one file named `Reborn-Server-Setup-1.0.N.exe`
(`1.0.N` is the version number). The owner gives it to you. It holds the Steam loader, so
keep it private and never post it anywhere public.

1. **Disk space.** Make sure the drive you install to has at least 50 GB free.
2. **Install Battleborn with Steam** on that machine. Sign in to Steam, find Battleborn in
   your library, and install it. When it is done, **close Steam completely** (right-click
   the Steam icon by the clock, then Exit). The server uses its own small loader, and a
   running Steam can get in its way.
3. **Run `Reborn-Server-Setup-1.0.N.exe`.** It looks for the game by itself. If it can't
   find it, press Browse and pick the main Battleborn folder (the one that has `Binaries`
   inside it). It will not install into a folder that is not Battleborn.
4. **Pick your options** on the page with the tick boxes (all are optional):
   - **Let players through Windows Firewall (UDP 7777-7779 and TCP 5000).** Ticked. On a
     server that already has Reborn, it keeps the ports you set in the Control Panel.
   - **Install the Visual C++ 2012 runtime the game needs** (a Microsoft add-on). Ticked only
     if this machine does not have it yet.
   - **Start matchmaking when I sign in to Windows.** Ticked.
   - **Put "Reborn Control Panel" on the desktop.** Ticked.
   - **Free up space: remove game files the server does not need (tick the parts below).**
     Shown only when there are parts to offer. Each part is unticked, with its size next to it. Ticking one deletes
     those game files. A server does not need them, and we tested that games still run
     without them. **Removed files only come back by copying the game again** (in Steam:
     Verify integrity of game files). If the page says "Tested one at a time", do not tick
     several parts together unless you are happy to test that yourself.
5. **GitHub access (only on a server that does not have the GitHub program, `gh`).** The
   installer skips this page when `gh` is already signed in. Otherwise it asks for a
   read-only token for the owner's private builds, so the panel's **Updates** tab can fetch
   new versions without `gh`. You can leave it empty and add it later. The token is kept
   safe for your Windows user only and is never shown again.
6. If the installer could not do something (for example open the firewall), the last page
   lists each step that did not work and what to run by hand. The install still finishes,
   and the other steps still run.
7. If Windows Defender removes `steamclient_loader_x64.exe`, allow it in Windows Security.
   Loaders like this are often flagged even though they are harmless.
8. **Open the hosting company's firewall too.** On the company's website (its control
   panel), allow incoming **UDP ports 7777 to 7779** and **TCP port 5000**.
   This step can't be done from inside the machine.

**Updating later:** use the Control Panel's **Updates** tab as before. Running a newer
installer over an old one also works and keeps your users, keys and settings. If you run an
older installer than what is on the server, it asks first (the default answer is No).

**Uninstalling:** use Windows' Apps list. It asks **Also delete users and keys?** The
default is **No**, so your user list and keys stay unless you say Yes. Files removed to save
space are not put back. The copies of the Steam loader that matchmaking made for each game
server (the `rb_ids` folder) are removed too.

## Without the installer

The old way still works.

1. Do steps 1 and 2 above (disk space, Battleborn from Steam, close Steam).
2. **Copy the server kit** (`Reborn-ServerKit-<version>.zip`) to the machine.
3. **Unzip it into the game's `Binaries\Win64` folder.** In Steam that is: right-click
   Battleborn, Manage, Browse local files, then `Binaries`, then `Win64`. You should see
   `Battleborn.exe` there. Say yes to replacing files.
4. If Windows Defender removes `steamclient_loader_x64.exe`, allow it in Windows Security.
5. **Double-click `OpenFirewall.bat`** and say yes when Windows asks for administrator
   rights. This lets players through Windows Firewall.
6. **Open the hosting company's firewall too** (step 8 above).

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
registered and `reborn-key.txt` is the file you sent, then close the game and start it again (the
game reads both files only when it starts). Otherwise add them again with a new key.

To change a name: `python -m tools.vm user rename <old> <new>`. The player must then use the new
name in `reborn-player.txt`.

"Require keys to join" is on by default. `python -m tools.vm user require-keys off` lets anyone join
(on again with `on`). `python -m tools.vm --help` lists every command.

**Upgrading from the old setup:** the first time the service starts with old `"StartKeys"` in its
settings and no user list, it turns them into users who can start games. After that first start,
delete the `StartKeys` block from `appsettings.json` (the log reminds you). While it stays there,
a missing user list is rebuilt from it, which brings back anyone you removed. Those users work with their key alone until you rename them to the player's real
name with `user rename <old> <new>`; after that the player's `reborn-player.txt` must match.

The timeouts are `"RequestedGame": { "NoJoinSeconds": 180, "EmptySeconds": 60 }` in the matchmaking
`appsettings.json`. `matchmaking.log` records which player started each game.

## Control Panel

The Control Panel is a window that shows you what your server is doing and lets you change
things without editing any files. It replaces most of the `python -m tools.vm user ...`
commands above.

**It only works on the server machine itself.** Open it over Remote Desktop, on the rented
machine. It will not work from your own PC, because it talks to the matchmaking service (the
program that lists games and lets players join them) that runs on the server.

**To open it,** double-click the **Reborn Control Panel** icon on the server's desktop. The
update puts the icon there the first time it installs the panel. If you can't see it, open
`ControlPanel\ControlPanel.exe` inside the game's `Binaries\Win64` folder (the folder with
`Battleborn.exe` in it). If you update from the command line instead of the **Updates** tab,
**close the panel first.** Windows cannot replace a program while it is open, and the update
will tell you to close it. The **Updates** tab closes and reopens the panel for you.

**The top of the window** shows:

- the server's address, with a **Copy** button. Friends don't need it: their zip already knows
  where the server is;
- a green light when matchmaking is running, and a red one when it is not. When it is not,
  press **Start matchmaking** in the yellow bar;
- how many games are running, and a memory estimate for them.

**The tabs:**

- **Live games.** Every game running now, and who is playing which hero.
- **Users.** Everyone who may join.
  - To add someone, type their name, press **Generate Key**, and press **Save**. Tick
    **Can create games** if they may start games.
  - On a person's row, **Make zip** builds their personal download. It has the game files and
    their key in it. **New key** gives them a fresh key. **Remove** takes them off the list.
  - The row also shows when you made their zip and when they were last seen.
  - The panel will not let you remove the last person who can start games. That would lock
    you out.
  - **Rename** changes a person's name at once. They then need a new zip with the new name.
    Until they install it, they can't join. If they are in a game, they are dropped.
  - **Confirm name** shows instead of Rename for people brought over from the old key files.
    Their row says "confirm name". The name stored for them is only a label. Type the name they
    play with, exactly as it appears in the game. If it doesn't match, they are dropped and can't
    join until you fix it. **Make zip** stays off on their row until you confirm the name.
  - **Seen recently but not on the list** shows names that joined a game lately but match
    nobody on the list, newest first. Look here for a person's real name before you confirm it.
  - **Require keys to join** is the switch for the whole server. On: a player needs the key in
    their zip. Off: a name is enough, and anyone who finds the server can join. The panel asks
    before you change it. Turning it on drops players without a key from their games.
  - **No user list yet?** Then anyone can join, and the Users tab says so. The first person you
    add starts the list. From then on, only people on the list can join. So add yourself first,
    with **Can create games** ticked.
- **Ports.** The ports your games use. Each game running at the same time needs its own port.
  - Add or remove ports, then press **Save**. The change is used the next time matchmaking
    starts. Press **Restart matchmaking** to use it now. That stops every game that is running.
  - **Save** asks Windows for permission once, to open the same ports in Windows Firewall.
    Say yes. This is the normal Windows "Do you want to allow this app to make changes?" box
    (it is called UAC).
  - **The hosting company's firewall is separate.** The panel cannot open it. Open the same
    ports on the company's website, as in the one-time setup above.
  - The tab also tells you how much memory your ports need. Each game takes about 1.3 GB.
- **Logs.** The end of the matchmaking log, newest at the bottom. **Refresh** reads it again.
  **Open game logs folder** opens the folder with the game servers' own logs.
- **Updates.** Puts a new version of Reborn on this server, and takes it back if you change
  your mind. See **Updating the server from the panel** below.

**Where friend zips are saved.** **Make zip** saves the file in
`Documents\Reborn Friend Zips` on the server machine, named `Reborn-Mod-Installer_<name>.zip`.
The Users tab has an **Open zip folder** button. Copy the zip off the machine and send it to
that friend only. It holds their personal key and the Steam loader, so never post it anywhere
public. The panel makes it from
`reborn-player-installer.zip` in the game's `Binaries\Win64` folder, which every update refreshes.

Updates never touch your user list, your "last seen" list, your record of made zips, your
matchmaking settings, or the panel's private password file (the small file that lets the
panel talk to matchmaking).

## Updating the server from the panel

New versions of Reborn have numbers like **1.0.4**. The owner makes them on their own PC.
Each one is signed, so this server only accepts a version that really came from the owner.
The **Updates** tab does the rest. You never copy files by hand.

**What you need.** The server machine must be able to reach GitHub, with the GitHub program
(`gh`) signed in. If it can't, the tab says: "This server can't reach the builds on GitHub
right now. Nothing was changed."

**The very first time.** The **Updates** tab comes with the first numbered release, so that
one (1.0.1) can't be installed from the tab. Install it on the server with
`python -m tools.vm update`. Every later version goes in with **Update now**.

**Look for a new version.**

1. Open the **Updates** tab. It shows **Installed version**.
2. The panel looks for a new version by itself when it opens, and again every 6 hours. Press
   **Check for updates** to look right now.
3. If there is one, the tab says **Update available** with the new number, and a **What's
   new** note. The same line shows at the top of the window.

**Install it.**

1. Press **Update now**.
2. If players are in games, the panel asks you first, for example: "2 players are in games.
   Updating stops those games. Update now?" Say No to wait. Nothing changes until you say Yes.
3. The panel downloads the update and checks it is exactly the one the owner signed. Then it
   **closes by itself.** A small update window opens and shows what it is doing. Leave it alone.
4. The update window stops matchmaking and the games, puts the new files in, and starts
   matchmaking again. The **Control Panel opens again by itself** when it is done. This
   usually takes a minute or two.
5. The tab shows the result under **Last update**, for example "Updated to 1.0.4. A game is up
   and running."

**If the new version does not work.** After the update, the server waits up to 3 minutes for
a game server to start and report that it is ready. If none does, it puts the old version back
and starts matchmaking again by itself. **Last update** then says what went wrong, in a warning colour, and "Details" has the
technical part. Your players can join again as soon as matchmaking is back.

**Go back to an older version.** Under **Saved versions**, each earlier version has a **Roll
back** button. Press it, answer the question ("Go back to 1.0.3? The server restarts."), and
the server goes back the same way an update goes forward. If players are in games, the
question says so first, as it does for **Update now**. Use this if a new version works but
you don't like it.

**Saved versions and disk space.** Every update first saves a copy of the version it replaces,
in the `RebornVersions` folder inside `Binaries\Win64`. The last **3** are kept. Older ones are
deleted by themselves. Each one takes roughly **150 MB**, so the three together are about
**450 MB**. Make sure the drive has room for them. While an update runs, the folder also holds
the unpacked download (a few hundred MB more). It is deleted once the update has worked. After
an update that did not work, it stays until the next **Update now** replaces it.

**If an update did not finish.** If the machine switched off, or someone closed the update
window part way through, the **Updates** tab shows a warning: the server may be a mix of two
versions. It then offers **one** button, **Roll back to** the version you had before. Press
it. **Update now** and the list of saved versions stay switched off until you do.
If the saved copy of that version is gone, there is no button. The warning names a file
(`in-progress.json`) instead. Delete that file and check the server by hand.

**What an update never touches.** The player names and keys on your user list, the "last seen"
list, your record of made zips, `appsettings.json` (your games and ports) and the panel's
private password file all stay exactly as they were. Your friends' own name, key and Steam
files on their PCs are not touched either (see the friends' note below).

**Friends and the in-game update.** The server also hands out the new game files to players.
A player who has the update system sees **Update available: Reborn 1.0.4** in the game's main
menu, and can press **Download**. But a friend's game can only do that if it came from a zip
made **after** the first numbered release. Zips made earlier don't know how to update, so each
friend needs **one last zip**:

1. Update the server to the first numbered release (1.0.1) with `python -m tools.vm update`
   (see "The very first time" above).
2. On the **Users** tab, press **Make zip** on each friend's row, and send them the new zip.
3. They run its `Install.bat`. From then on their game updates itself.

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

The easy way is the **Updates** tab in the Control Panel (see above). The old way still
works: copy the new server kit over and unzip it into `Win64` again, replacing files. Stop the
server first, or Windows won't let the files be replaced.

## When something goes wrong

The server's logs are in `Documents\RebornLogs`, in a folder that starts with
`reborn-server-`. `reborn-server.log` is everything the server printed. If it crashed, the
same folder has a crash dump (`.dmp`) and a short crash report (`.json`).
`CollectLogs.bat` zips the newest folder onto the Desktop.

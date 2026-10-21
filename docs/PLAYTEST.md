# Battleborn Reborn: two-player playtest

This is a test build. The goal right now is simple: two people, on two different
computers, playing a story mission together over the internet. Things will break.
When they do, the logs tell us why, so please send them.

## For the friend joining the game

### What you need

1. Your own copy of Battleborn installed through Steam (the final Steam version).
2. Windows 10 or 11, 64-bit, or Linux with Wine or Proton (see "Playing on Linux" below).
3. The playtest zip from the host (`Reborn-Playtest-<version>.zip`).
4. The Steam loader files. Battleborn's online service is gone, so the game is started
   through a small loader instead of Steam. If your zip is named `...-with-loader.zip`,
   you already have them. If not, ask the host for these four files:
   `steamclient_loader_x64.exe`, `steamclient.dll`, `steamclient64.dll`,
   `ColdClientLoader.ini`.

### Install (once, and again for every new build)

Extract the zip, then double-click `Install.bat` in the extracted folder. It finds the game,
tells Windows Defender to leave the game folder alone, installs the Visual C++ 2012 runtime if
missing, copies the mod in, asks for your player name the first time, and puts a
**Battleborn Reborn** shortcut on the desktop. The zip's `README.txt` (docs/PLAYER-SETUP.md) has
the full steps and a manual way.

### Play

1. Double-click `steamclient_loader_x64.exe` in the `Win64` folder. Do not start the game
   from Steam; the mod needs the loader.
2. A black console window opens next to the game. Leave it open; that is the mod talking.
3. At the main menu, choose a public mode (Story or Versus). A server browser opens and
   lists the games running on the Reborn server, with how many players are in each.
4. Click **Join** on a game that says **Joinable!**. (Or click **Direct Connect** and type
   an address a friend gives you.)
5. Pick your hero, skin and taunt on the game's own hero-select screen, and play. If you run out
   of time, the timer picks for you. A small **Gear** box (bottom-right) holds your three gear
   choices. Everyone sees the name you typed the first time you ran the game (saved in
   `reborn-player.txt` next to the game).
6. The game waits up to 2 minutes for everyone to join. After that a Story game starts anyway,
   and a Versus game is cancelled if not enough people came (you go back to the main menu).

If the list is empty, click **Refresh Server Browser**; if it stays empty, the Reborn
server is down or your network blocks it. Direct Connect still works. To use a different
matchmaking server, add `-rbcoordinator=ADDRESS:5000` to the game's launch options.

### Playing on Linux (Wine or Proton)

The mod rides inside a file called `dxgi.dll`. Wine normally ignores a `dxgi.dll` in the
game folder and uses its own, so the mod never starts. You can tell: no black mod console
appears, and Direct Connect does nothing useful. Install the same way as above, then:

- **Proton (Steam):** in Steam, add `steamclient_loader_x64.exe` as a non-Steam game,
  force a Proton version in its Compatibility settings, and set its launch options to
  `WINEDLLOVERRIDES="dxgi=n,b" %command%`. Start it from Steam.
- **Plain Wine, Lutris or Bottles:** use a 64-bit prefix with DXVK installed. Add a DLL
  override for `dxgi` set to "native, then builtin" (in `winecfg`, Libraries tab, or the
  runner's DLL overrides), or start it with
  `WINEDLLOVERRIDES="dxgi=n,b" wine steamclient_loader_x64.exe` from the `Win64` folder.

The mod's log notes `running under Wine` near the top, so the host can tell. Your logs
are inside the Wine prefix: `drive_c/users/<your name>/Documents/RebornLogs` (under
Proton the name is `steamuser`, and the prefix is in `steamapps/compatdata/<id>/pfx`).
`CollectLogs.bat` may not be able to zip them under Wine; if it says so, zip the newest
folder there yourself and send it.

### When something goes wrong

Write down roughly what time it happened and what you were doing. Then:

1. Close the game if it is still running.
2. Double-click `CollectLogs.bat` in the `Win64` folder.
3. It puts a file called `RebornLogs-....zip` on your Desktop. Send that file to the host,
   with a screenshot if you took one.

Your logs live in `Documents\RebornLogs`, one folder per game session. If the game
crashed, the folder also holds a crash dump (`.dmp`) and a short crash report (`.json`).
Those are the most useful things you can send.

Nothing in the logs is personal beyond your Windows user name in file paths.

## "Improper Credentials!"

If the game shows a **Reborn** notice saying **Improper Credentials!**, the server did not let you
in. Click **OK**, then check these:

1. Open `reborn-player.txt` in the `Win64` folder. The name in it must be exactly the name the host
   registered for you.
2. Check that `reborn-key.txt` is in the same folder and is the file the host sent you.
3. Still refused? The host may have removed you, or the key may be wrong. Ask the host for a new key.

## Starting your own game on the host's server

Joining the host's server needs your `reborn-key.txt` (from the host) in the game's `Binaries\Win64`
folder, next to `reborn-player.txt` (your name). To start games, the host also has to tick "can
create games" for you; nothing changes on your PC. In the server browser, click **Start a Game**. Leave **Run on** as **the
server**, pick a map and mode, how many players to wait for, and for Versus the bots on each team
and a draft style (Quick Select, Ban Pick, Captain's Draft, Chaos Rumble or Classic). Give the
game a name and click **Start**. You join automatically when the server is ready; friends find the
game in the browser. The first time, the game asks for a display name.

## For the host

### Start a game from inside Battleborn

No Python needed. This works for solo play too: set the players to 1.

1. Start the game with `steamclient_loader_x64.exe` as usual.
2. At the main menu, choose a public mode (Story, Operations or Versus). The server browser opens.
3. Click **Start a Game**.
4. Set **Run on** to **my PC**. Pick the mission or map, and how many players to wait for
   (including you). For Versus you can also set the bots on each team and a draft style. The
   game waits up to 2 minutes for everyone; Story then starts anyway, and Versus is cancelled if
   not enough people came.
5. Click **Start**. The mod starts a server on your PC in the background. This takes a minute
   or two, then your game joins it by itself.
6. Friends join with **Direct Connect** to your address (see "Let your friend reach you" below).

The server's window stays minimised on the taskbar. It closes when the match ends, when you close
your game, or when you host another game. If Windows asks whether to allow `Serverborn.exe`
through the firewall, say yes, or friends cannot reach you. That prompt can hide behind the game:
check the taskbar.

### Start a game with Python

From the repo folder, with the mod built and copied into your game folder:

```
python -m tools.host
```

This starts the server on The Algorithm, waiting for 2 players, and starts your own game
beside it. In your game, use **Direct Connect** with `127.0.0.1`.

Other missions: `--map Portal_P` (Void's Edge), `Captains_P` (The Renegade),
`Evacuation_P` (The Archive), `Ruins_P` (Sentinel), `Observatory_p` (The Experiment),
`Refinery_P` (The Saboteur), `Cathedral_P` (Heliophage), `PvE_Prologue_P` (Prologue).
`--no-client` starts the server only.

### Let your friend reach you

The server listens on UDP port 7777. Pick one:

- **Easiest: a virtual LAN** such as Tailscale or ZeroTier. Both of you install it and
  join the same network. Give your friend your Tailscale/ZeroTier address (for example
  `100.x.y.z`). No router changes needed.
- **Port forwarding:** forward UDP 7777 on your router to this PC and give your friend
  your public IP address. Allow `Serverborn.exe` through Windows Firewall when asked.

### Make the download

```
python -m tools.package
```

This builds the mod and writes `dist/Reborn-Playtest-<commit>.zip`: `Install.bat` and the guide
on top, the game-folder files under `files/`. Add `--with-loader` to also include the Steam loader
files from your game folder (started with the mod's console minimized); send that version to
players directly rather than posting it publicly. A mod built with a `LocalSettings.hpp` server
address carries that address inside `reborn.dll`, so share such zips privately too.

### Read a friend's logs

Unzip their `RebornLogs-....zip`. `game.log` is everything the mod printed. Crashes have
a `game.<n>.json` summary and `game.<n>.dmp`. Your own logs for the same session are in
`Documents\RebornLogs\host-server-...` and `host-player-...`; line them up by time.

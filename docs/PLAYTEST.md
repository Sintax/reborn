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

1. In Steam, right-click Battleborn, then Manage, then Browse local files.
2. Open `Binaries`, then `Win64`. You should see `Battleborn.exe` there.
3. Unzip everything from the playtest zip into that `Win64` folder. Say yes to replacing
   files.
4. If Windows Defender or your antivirus removes `steamclient_loader_x64.exe`, allow it.
   Loaders like this are often flagged even though they are harmless.

### Play

1. Double-click `steamclient_loader_x64.exe` in the `Win64` folder. Do not start the game
   from Steam; the mod needs the loader.
2. A black console window opens next to the game. Leave it open; that is the mod talking.
3. At the main menu, choose a public mode (Story or Versus). A server browser opens.
4. Click **Direct Connect**, type the address the host gives you, and click **Start!**
5. Pick your hero, lock in, and play.

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

## For the host

### Start a game

From the repo folder, with the latest build already deployed by the debug loop:

```
python -m debugloop.host
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
python -m debugloop.package
```

This builds the mod and writes `dist/Reborn-Playtest-<commit>.zip`. Add `--with-loader`
to also include the Steam loader files from your game folder; send that version to the
friend directly rather than posting it publicly.

### Read a friend's logs

Unzip their `RebornLogs-....zip`. `game.log` is everything the mod printed. Crashes have
a `game.<n>.json` summary and `game.<n>.dmp`. Your own logs for the same session are in
`Documents\RebornLogs\host-server-...` and `host-player-...`; line them up by time.

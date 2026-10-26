# Battleborn Reborn: setup for {NAME}

This zip lets you play Battleborn online again, on your host's server. Everything you need is
inside, including your own server key and your player name. You do not need any address: the
game already knows where the server is.

Please don't share this zip. Your key is yours, and the host can switch it off.

## What you need first

- Battleborn installed through Steam (the last Steam version). It is about 45 GB.
- Windows 10 or 11, 64-bit.

## Easy setup: let Install.bat do it

1. Right-click this zip and choose **Extract All**, then **Extract**. (Do not run anything from
   inside the zip without extracting it.)
2. In the extracted folder, double-click **`Install.bat`**.
   - If Windows says **"Windows protected your PC"**, click **More info**, then **Run anyway**.
   - When Windows asks **"Do you want to allow this app to make changes to your device?"**, click
     **Yes**. Setup needs this to talk to Windows Defender and install a Microsoft runtime.
3. A blue window shows each step and ends with **All done**. Press Enter to close it.

What it does, in order:

1. Finds Battleborn's game folder through Steam. If it cannot, it asks you to pick the folder that
   contains `Battleborn.exe` (Steam > Battleborn > Manage > Browse local files > Binaries > Win64).
2. Tells Windows Defender to skip that one folder (and the setup folder), so it stops deleting the
   loader. Everything else on your PC stays protected.
3. Installs Microsoft's Visual C++ 2012 runtime (64-bit) if it is missing. Battleborn needs it
   (without it Windows says **"MSVCP110.dll was not found"**). It downloads straight from Microsoft
   and only runs the file if Microsoft's signature checks out.
4. Copies the mod, the loader and your settings into the game folder. Any file it replaces is
   saved first in `Binaries\Win64\rb_backups`.
5. Puts a **Battleborn Reborn** shortcut on your desktop.

If it says Defender removed a file before setup could protect it, delete the extracted folder,
extract the zip again into the same place, and run `Install.bat` again.

## Manual setup (only if Install.bat cannot run)

**A. Visual C++ 2012 runtime.** Download and run Microsoft's installer, tick the box to agree,
click **Install**:
https://download.microsoft.com/download/1/6/B/16B06F60-3B20-4FF2-B699-5E9B7962F9AE/VSU_4/vcredist_x64.exe
(Microsoft's list: https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist, under
"Visual Studio 2012 (VC++ 11.0) Update 4", the X64 file.)

**B. Find the game folder.** In Steam, right-click **Battleborn**, then **Manage**, then **Browse
local files**. Open `Binaries`, then `Win64`. You are in the right place when you see
`Battleborn.exe`.

**C. Stop Windows Defender from deleting the loader.** Do this before copying.

1. Open the Start menu, type **Windows Security**, and open it.
2. Click **Virus & threat protection**.
3. Under **Virus & threat protection settings**, click **Manage settings**.
4. Scroll down to **Exclusions** and click **Add or remove exclusions**. Click **Yes** if Windows
   asks for permission.
5. Click **Add an exclusion**, choose **Folder**, and pick the `Win64` folder from B.

If Defender already removed the loader: in Windows Security, open **Virus & threat protection**,
then **Protection history**, click the entry that mentions `steamclient_loader_x64.exe` (or
`steamclient64.dll`), then **Actions** and **Restore** (or **Allow on device**).

**D. Copy the files.** Copy everything inside this zip's `files` folder (not the folder itself)
into `Win64`, including the `steam_settings` folder. If Windows asks about replacing files, choose
**Replace the files in the destination**. Afterwards `Win64` contains, among the game's own files:

- `steamclient_loader_x64.exe`, `steamclient.dll`, `steamclient64.dll`, `ColdClientLoader.ini`
- `dxgi.dll` and `reborn.dll` (the mod)
- `reborn-key.txt` (your server key) and `reborn-player.txt` (your player name)
- `steam_settings` (a folder with two small settings files)
- `CollectLogs.bat`

## Start the game

Double-click the **Battleborn Reborn** shortcut on your desktop (or `steamclient_loader_x64.exe`
in the `Win64` folder).

- Do not start Battleborn from Steam. It will not go online without the loader.
- The mod also opens a black text window, which starts minimized on the taskbar so only the game
  shows. Leave it running; that is the mod at work. It closes with the game.
- If Windows Firewall asks whether to allow Battleborn, click **Allow**.
- If Windows says **"Windows protected your PC"** the first time, click **More info**, then
  **Run anyway**. That only happens once.

## Play

1. At the main menu, choose **Story**, **Operations** or **Versus**. The server browser opens and
   lists the games running on the server.
2. **Join a game:** click **Join** on a game marked **Joinable!**.
3. **Start your own game:** (the host must allow it for you) click **Start a Game**. Leave **Run on** as **the server**, pick a map
   and mode, how many players to wait for (1 to play alone right away), and for Versus the bots
   on each team and a draft style (Quick Select, Ban Pick, Captain's Draft, Chaos Rumble or
   Classic). Give the game a name, then click **Start**. When the server is ready, you join it
   automatically. Friends see your game in the browser and can join.
4. Pick your hero, skin and taunt on the game's own hero-select screen, and play. If you run out
   of time, the timer picks for you (in a random-hero game it picks for everyone). A small
   **Gear** box in the bottom-right corner holds your three gear choices. Everyone sees the name
   in your `reborn-player.txt`.
5. The game waits up to 2 minutes for everyone to join. After that a Story game starts anyway,
   and a Versus game is cancelled if not enough people came (you go back to the main menu).

If the list stays empty after **Refresh Server Browser**, the server may be down. Tell the host.

## "Improper Credentials!"

If the game shows a **Reborn** notice saying **Improper Credentials!**, the server did not let you
in. Click **OK**, then check these:

1. Open `reborn-player.txt` in the `Win64` folder. The name in it must be exactly the name the host
   registered for you.
2. Check that `reborn-key.txt` is in the same folder and is the file from this zip.
3. If you changed either file, close the game and start it again: the game reads them only when
   it starts. Then try to join again.
4. Still refused? The host may have removed you, or the key may be wrong. Ask the host for a new key.

## If something goes wrong

1. Close the game if it is still open.
2. Double-click `CollectLogs.bat` in the `Win64` folder.
3. It saves a `RebornLogs-....zip` on your Desktop. Send that file to the host, with roughly what
   time it happened and what you were doing.

## Updates

If your game came from a recent zip, it can update itself. You do not have to run
`Install.bat` again for a normal update.

1. When you reach the main menu, the game checks for a new version. This happens quietly,
   once each time you start the game.
2. If there is one, a **Reborn** box appears in the middle of the menu. It says **Update
   available: Reborn 1.0.4**, and a short note about what changed.
3. Press **Download**. (Press **Not now** to hide the box. It comes back the next time you
   start the game.)
4. The game downloads the new files and puts them in place. This takes a few seconds. Then
   the box says **Update ready. Restart the game to use it.** Press **OK**.
5. Close the game and start it again. You are now on the new version.

If something goes wrong, the box says "The update could not be installed" and tells you why.
Press **OK**. Your game is still the old version, and you can keep playing.
If the new version does not start at all, the game puts the old one back by itself the next time
you start it.

**Nothing personal changes.** An update only replaces the two game files of the mod
(`reborn.dll` and `dxgi.dll`). Your name (`reborn-player.txt`), your key (`reborn-key.txt`)
and your Steam files stay exactly as they are.

**Older zips cannot do this.** If your zip is from before updates were added, your game
does not know how to update itself yet. Ask the host for a new zip once, and run its
`Install.bat`. After that, updates come by themselves.

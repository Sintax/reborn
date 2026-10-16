# Battleborn Reborn: player setup

This zip lets you play Battleborn online again on the Reborn server. You do not need any address:
the game already knows where the server is.

## What you need first

- Battleborn installed through Steam (the last Steam version). It is about 45 GB.
- Windows 10 or 11, 64-bit. (Linux: see "Playing on Linux" at the end.)
- The Steam loader. Battleborn's online service is gone, so the game is started through a small
  loader instead of Steam. If this zip's `files` folder has `steamclient_loader_x64.exe`, you
  already have it. If not, ask the host for these four files: `steamclient_loader_x64.exe`,
  `steamclient.dll`, `steamclient64.dll`, `ColdClientLoader.ini`.

## Easy setup: let Install.bat do it

1. Right-click this zip and choose **Extract All**, then **Extract**. (Do not run anything from
   inside the zip without extracting it.)
2. In the extracted folder, double-click **`Install.bat`**.
   - If Windows says **"Windows protected your PC"**, click **More info**, then **Run anyway**.
   - When Windows asks **"Do you want to allow this app to make changes to your device?"**, click
     **Yes**. Setup needs this to talk to Windows Defender and install a Microsoft runtime.
3. The first time, it asks for your player name: 1 to 24 letters, digits, `-` or `_`.
4. A blue window shows each step and ends with **All done**. Press Enter to close it.

What it does, in order:

1. Finds Battleborn's game folder through Steam. If it cannot, it asks you to pick the folder that
   contains `Battleborn.exe` (Steam > Battleborn > Manage > Browse local files > Binaries > Win64).
2. Tells Windows Defender to skip that one folder (and the setup folder), so it stops deleting the
   loader. Everything else on your PC stays protected.
3. Installs Microsoft's Visual C++ 2012 runtime (64-bit) if it is missing. Battleborn needs it
   (without it Windows says **"MSVCP110.dll was not found"**). It downloads straight from Microsoft
   and only runs the file if Microsoft's signature checks out.
4. Copies the mod (and the loader, if this zip has it) into the game folder. Any file it replaces
   is saved first in `Binaries\Win64\rb_backups`.
5. Sets up your player name and a Steam identity of your own (only the first time; updates keep
   them).
6. Puts a **Battleborn Reborn** shortcut on your desktop. Without the loader it tells you which
   files to get instead: put them in the game folder and run `Install.bat` again.

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
**Replace the files in the destination**. Add the loader files too if they came separately.

**E. Your name.** In `Win64\steam_settings`, create a text file `configs.user.ini` with these
lines, using your own name and changing the last digits of the number to something random:

```
[user::general]
account_name=YourName
account_steamid=76561197960512345
language=english
```

## Start the game

Double-click the **Battleborn Reborn** shortcut on your desktop (or `steamclient_loader_x64.exe`
in the `Win64` folder).

- Do not start Battleborn from Steam. It will not go online without the loader.
- The mod also opens a black text window. With the loader from this zip it starts minimized on the
  taskbar so only the game shows. Leave it running; that is the mod at work. It closes with the game.
- If Windows Firewall asks whether to allow Battleborn, click **Allow**.
- If Windows says **"Windows protected your PC"** the first time, click **More info**, then
  **Run anyway**. That only happens once.

## Play

1. At the main menu, choose **Story**, **Operations** or **Versus**. The server browser opens and
   lists the games running on the server.
2. **Join a game:** click **Join** on a game marked **Joinable!**.
3. Pick your hero, lock in, and play. A match starts once enough players are in.

If the list stays empty after **Refresh Server Browser**, the server may be down. Tell the host.

**Starting your own game** needs a server key from the host (`reborn-key.txt`). Put it in the
`Win64` folder. Then **Start Server Game** in the server browser lets you pick a map and mode, how
many players to wait for (1 to play alone), and bots for Versus. You join automatically when the
server is ready, and others see your game in the browser.

## If something goes wrong

1. Close the game if it is still open.
2. Double-click `CollectLogs.bat` in the `Win64` folder.
3. It saves a `RebornLogs-....zip` on your Desktop. Send that file to the host, with roughly what
   time it happened and what you were doing.

## Updates

When the host sends a new zip, extract it and run its `Install.bat` again.

## Playing on Linux (Wine or Proton)

`Install.bat` is for Windows. On Linux, copy the `files` folder's contents into `Win64` by hand
(step D and E above). The mod rides inside `dxgi.dll`, which Wine normally ignores, so:

- **Proton (Steam):** add `steamclient_loader_x64.exe` as a non-Steam game, force a Proton version
  in its Compatibility settings, and set its launch options to `WINEDLLOVERRIDES="dxgi=n,b" %command%`.
- **Plain Wine, Lutris or Bottles:** use a 64-bit prefix with DXVK installed and set a DLL override
  for `dxgi` to "native, then builtin", or start it with
  `WINEDLLOVERRIDES="dxgi=n,b" wine steamclient_loader_x64.exe` from the `Win64` folder.

If no black mod window appears, the override is not active. Logs are inside the prefix at
`drive_c/users/<your name>/Documents/RebornLogs`; zip the newest folder yourself if
`CollectLogs.bat` cannot.

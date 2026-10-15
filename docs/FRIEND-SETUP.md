# Battleborn Reborn: setup for {NAME}

This zip lets you play Battleborn online again, on your host's server. Everything you need is
inside, including your own server key and your player name. You do not need any address: the
game already knows where the server is.

Please don't share this zip. Your key is yours, and the host can switch it off.

## What you need first

- Battleborn installed through Steam (the last Steam version). It is about 45 GB.
- Windows 10 or 11, 64-bit.

## Step 1: Stop Windows Defender from deleting the loader (do this first)

The game starts through a small program called `steamclient_loader_x64.exe`, because
Battleborn's own online service is gone. Programs like it are often flagged as a threat even
though they are harmless, and Windows Defender may delete it the moment you unzip it. Telling
Defender to leave the game's folder alone prevents that.

1. Find the folder (see Step 2, points 1 to 3) and keep that window open.
2. Open the Start menu, type **Windows Security**, and open it.
3. Click **Virus & threat protection**.
4. Under **Virus & threat protection settings**, click **Manage settings**.
5. Scroll down to **Exclusions** and click **Add or remove exclusions**. Click **Yes** if
   Windows asks for permission.
6. Click **Add an exclusion**, choose **Folder**, and pick the game's `Win64` folder from Step 2.

Defender will now ignore that one folder only. Everything else stays protected.

**If Defender already removed the loader:**

1. In Windows Security, click **Virus & threat protection**, then **Protection history**.
2. Click the entry that mentions `steamclient_loader_x64.exe` (or `steamclient64.dll`).
3. Click **Actions**, then **Restore** (or **Allow on device**).
4. If it does not come back, add the exclusion above and unzip this zip into the folder again.

**If Windows says "Windows protected your PC"** the first time you start the loader, click
**More info**, then **Run anyway**. That only happens once.

## Step 2: Where to copy the files

1. Open Steam and go to your Library.
2. Right-click **Battleborn**, then **Manage**, then **Browse local files**.
3. Open the `Binaries` folder, then the `Win64` folder. You are in the right place when you can
   see `Battleborn.exe`.
4. Copy everything from this zip into that `Win64` folder, including the `steam_settings`
   folder. If Windows asks about replacing files, choose **Replace the files in the destination**.

When you are done, `Win64` contains (among the game's own files):

- `steamclient_loader_x64.exe`, `steamclient.dll`, `steamclient64.dll`, `ColdClientLoader.ini`
- `dxgi.dll` and `reborn.dll` (the mod)
- `reborn-key.txt` (your server key) and `reborn-player.txt` (your player name)
- `steam_settings` (a folder with two small settings files)
- `CollectLogs.bat` and this guide

## Step 3: Start the game

Double-click **`steamclient_loader_x64.exe`** in the `Win64` folder.

- Do not start Battleborn from Steam. It will not go online without the loader.
- A black text window opens next to the game. Leave it open; that is the mod at work. It closes
  with the game.
- If Windows Firewall asks whether to allow Battleborn, click **Allow**.

Tip: right-click the loader, then **Send to**, then **Desktop (create shortcut)**, to start it
from your desktop next time.

## Step 4: Play

1. At the main menu, choose **Story**, **Operations** or **Versus**. The server browser opens and
   lists the games running on the server.
2. **Join a game:** click **Join** on a game marked **Joinable!**.
3. **Start your own game:** click **Start Server Game**, pick a map and mode, how many players to
   wait for (1 to play alone right away), and bots for Versus, then **Start**. When the server is
   ready, you join it automatically. Friends see your game in the browser and can join.
4. Pick your hero, lock in, and play. A match starts once the chosen number of players is in.

If the list stays empty after **Refresh Server Browser**, the server may be down. Tell the host.

## If something goes wrong

1. Close the game if it is still open.
2. Double-click `CollectLogs.bat` in the `Win64` folder.
3. It saves a `RebornLogs-....zip` on your Desktop. Send that file to the host, with roughly what
   time it happened and what you were doing.

## Updates

When the host sends a new zip, copy its files into the same `Win64` folder again and replace
the old ones.

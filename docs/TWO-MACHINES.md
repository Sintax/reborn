# Working across the laptop and the rented server (the VM)

The laptop is where code is written, built and played. The VM only runs the server. Code
never gets edited on the VM; it arrives from GitHub.

| | Laptop | VM |
|---|---|---|
| Address | | SERVER-IP-REMOVED |
| Game folder | `D:\SteamLibrary\steamapps\common\Battleborn` | `D:\Battleborn` |
| Logs | `Documents\RebornLogs` | `C:\Users\ServerUser\Documents\RebornLogs` |
| Builds the mod (C++) | yes | no (no Visual Studio) |
| Builds matchmaking (.NET 8) | yes | yes |
| Claude session | this one | "VM-Server (Battleborn)" |

## What travels, and how

- **Code, scripts, matchmaking:** through this GitHub repo. The VM checks out the exact commit
  and builds matchmaking itself, so nothing big is uploaded.
- **The mod** (`reborn.dll`, `reborn.pdb`, `dxgi.dll`, about 11 MB): the VM can't build it, so the
  laptop uploads it to the **private** repo `Sintax/reborn-builds` as a release named after the
  commit.
- **The Steam loader files** never travel. They are already on the VM. This repo is public, so
  they must never go into it or into a public release.

## Updating the server

1. On the laptop: commit and push.
2. On the laptop: `python -m debugloop.vm release` (builds the mod and uploads it).
3. Ask the VM session: "run `python -m debugloop.vm update` in the reborn clone and report the
   result." It gets the code, downloads the mod, builds matchmaking, then stops the server, swaps
   the files, restarts matchmaking and waits until a game is listed. It ends with `UPDATE OK` or
   `PROBLEM: ...`.

Downloads and builds happen before the server is stopped, so a failed update leaves the old
server running. The VM's list of games (`matchmaking\appsettings.json`, "Roster") is kept across
updates.

## One-time setup on the VM

1. `git clone https://github.com/Sintax/reborn.git` into `C:\Users\ServerUser\Documents\GitHub\reborn`.
2. `python -m pip install psutil`
3. Set the game folder for Python: `setx BB_GAME_DIR D:\Battleborn` (then open a new terminal).
4. Run `OpenFirewall.bat` in the game's `Binaries\Win64` once, if the update warns that a rule is
   missing. The hosting company's firewall must allow UDP 7777-7779 and TCP 5000.

## One-time setup on the laptop

`gh repo create Sintax/reborn-builds --private --add-readme`

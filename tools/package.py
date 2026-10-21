"""Build the player download: dist/Reborn-Playtest-<commit>.zip.

Every zip has a double-click installer (Install.bat) on top and the game-folder files under files/.

    python -m tools.package                # mod files, guide, log collector, installer
    python -m tools.package --with-loader  # also the Steam loader files from your game folder
                                               # (send it to players directly, never post it)
    python -m tools.package --friend Kaz   # everything one friend needs: loader, mod, their own
                                               # server key, player name and Steam identity, and a
                                               # guide (dist/friends/Reborn-Kaz-<commit>.zip)

Install.bat copies files/ into the game's Binaries\\Win64 folder. Player guide: docs/PLAYER-SETUP.md
(friends: docs/FRIEND-SETUP.md).
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
import zipfile
import zlib
from pathlib import Path

from . import build, config, launch

DIST = config.REPO / "dist"
DXGI_PROJECT = config.REPO / "dxgi" / "dxgi.vcxproj"
DXGI_DLL = config.REPO / "dxgi" / "x64" / "Release" / "dxgi.dll"   # msbuild on the project, not the .sln
LOADER_INI = "ColdClientLoader.ini"

COLLECT_LOGS_BAT = r"""@echo off
rem Zips your newest Reborn log folder onto the Desktop so you can send it.
setlocal
set "LOGS=%USERPROFILE%\Documents\RebornLogs"
if not exist "%LOGS%" ( echo No logs yet in %LOGS% & pause & exit /b 1 )
for /f "delims=" %%d in ('dir /b /ad /o-d "%LOGS%"') do ( set "NEWEST=%%d" & goto found )
:found
set "OUT=%USERPROFILE%\Desktop\RebornLogs-%NEWEST%.zip"
powershell -NoProfile -Command "Compress-Archive -Force -Path '%LOGS%\%NEWEST%','%USERPROFILE%\Documents\My Games\Battleborn\PoplarGame\Logs\Launch.log' -DestinationPath '%OUT%'" 2>nul
if not exist "%OUT%" powershell -NoProfile -Command "Compress-Archive -Force -Path '%LOGS%\%NEWEST%' -DestinationPath '%OUT%'" 2>nul
if not exist "%OUT%" goto nozip
echo Saved %OUT%
echo Send that file to the host.
pause
exit /b 0
:nozip
rem No PowerShell (for example under Wine on Linux): the player zips the folder themselves.
echo Could not make a zip here. Zip this folder yourself and send it to the host:
echo   %LOGS%\%NEWEST%
pause
exit /b 1
"""


def commit() -> str:
    return subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=config.REPO,
                          capture_output=True, text=True, check=True).stdout.strip()


def readme(version: str) -> str:
    doc = (config.REPO / "docs" / "PLAYER-SETUP.md").read_text(encoding="utf-8")
    return f"Battleborn Reborn {version}\r\n\r\n" + doc.replace("\n", "\r\n")


def build_all() -> None:
    for project in (config.MOD_PROJECT, DXGI_PROJECT):
        cmd = build.msbuild_command()
        cmd[1] = str(project)
        p = subprocess.run(cmd, capture_output=True, text=True)
        if p.returncode != 0:
            raise RuntimeError(f"build of {project.name} failed:\n{p.stdout}{p.stderr}")


def game_files(with_loader: bool, win64: Path = config.WIN64, mod_dll: Path = config.MOD_DLL,
               dxgi_dll: Path = DXGI_DLL) -> dict[str, bytes]:
    """Everything that goes into the player's Binaries\\Win64 folder, by path inside that folder."""
    files = {
        "dxgi.dll": dxgi_dll.read_bytes(),
        "reborn.dll": mod_dll.read_bytes(),
        "CollectLogs.bat": COLLECT_LOGS_BAT.replace("\n", "\r\n").encode(),
        # Lets the player reach servers on the internet (see launch.CONFIGS_MAIN_INI); no Steam files.
        "steam_settings/configs.main.ini": launch.CONFIGS_MAIN_INI.encode(),
    }
    if with_loader:
        for f in (*launch.LOADER_FILES, LOADER_INI):
            if not (win64 / f).exists():
                raise RuntimeError(f"{f} not found in {win64}")
        for f in launch.LOADER_FILES:
            files[f] = (win64 / f).read_bytes()
        # The loader starts the game with the mod's console minimized: just the game window shows.
        ini = (win64 / LOADER_INI).read_text(encoding="utf-8").replace("\r\n", "\n")
        files[LOADER_INI] = with_min_console(ini).replace("\n", "\r\n").encode()
    return files


def _installer_zip(out: Path, files: dict[str, bytes], guide_name: str, guide: str) -> Path:
    """Layout for the double-click installer: Install.bat/.ps1 and the guide on top, and everything
    that belongs in the game's Win64 folder under files/ (Install.ps1 copies it there)."""
    out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for script in ("Install.bat", "Install.ps1"):
            text = (FRIEND_FILES / script).read_text(encoding="ascii").replace("\r\n", "\n")
            z.writestr(script, text.replace("\n", "\r\n"))
        z.writestr(guide_name, guide)
        for name, data in files.items():
            z.writestr("files/" + name, data)
    return out


def make_zip(out: Path, version: str, with_loader: bool, win64: Path = config.WIN64,
             mod_dll: Path = config.MOD_DLL, dxgi_dll: Path = DXGI_DLL) -> Path:
    """The standard player download. Install.bat asks for the player's name and makes their own
    Steam identity on their PC. Without the loader it is fine to hand out widely; with it, send it
    to players directly."""
    return _installer_zip(out, game_files(with_loader, win64, mod_dll, dxgi_dll), "README.txt", readme(version))


def check_friend_name(name: str) -> str:
    """Names end up in file names and the server's key list: letters, digits, - and _ only."""
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,24}", name or ""):
        raise ValueError(f"friend name {name!r}: use 1-24 letters, digits, - or _")
    return name


def friend_steamid(name: str) -> int:
    """A Steam ID of the friend's own, so two players never share one on the server."""
    return launch.STEAMID_BASE + zlib.crc32(("friend:" + name).encode()) % 100000


def friend_readme(name: str, version: str) -> str:
    doc = (config.REPO / "docs" / "FRIEND-SETUP.md").read_text(encoding="utf-8").replace("{NAME}", name)
    return f"Battleborn Reborn {version}\r\n\r\n" + doc.replace("\n", "\r\n")


MIN_CONSOLE = "-rbminconsole"


def with_min_console(ini: str) -> str:
    """ColdClientLoader.ini text with -rbminconsole added to the game's command line (once), so the
    mod's console starts minimized and only the game window shows."""
    nl = "\r\n" if "\r\n" in ini else "\n"
    lines = ini.split(nl)
    for i, line in enumerate(lines):
        if line.startswith("ExeCommandLine="):
            args = line[len("ExeCommandLine="):].split()
            if MIN_CONSOLE not in args:
                args.append(MIN_CONSOLE)
            lines[i] = "ExeCommandLine=" + " ".join(args)
            return nl.join(lines)
    for i, line in enumerate(lines):
        if line.strip() == "[SteamClient]":
            lines.insert(i + 1, "ExeCommandLine=" + MIN_CONSOLE)
            return nl.join(lines)
    return ini + ("" if ini.endswith(nl) or not ini else nl) + "[SteamClient]" + nl + "ExeCommandLine=" + MIN_CONSOLE + nl


def make_friend_zip(out: Path, version: str, name: str, key: str, win64: Path = config.WIN64,
                    mod_dll: Path = config.MOD_DLL, dxgi_dll: Path = DXGI_DLL) -> Path:
    """Everything one friend needs, unzipped into their Binaries\\Win64. The mod already knows the
    server's address (built in from LocalSettings.hpp), so no address travels as text. Send it to
    that friend only: it carries the Steam loader and their personal server key."""
    check_friend_name(name)
    files = game_files(True, win64, mod_dll, dxgi_dll)
    files["steam_settings/configs.user.ini"] = (
        f"[user::general]\r\naccount_name={name}\r\naccount_steamid={friend_steamid(name)}\r\n"
        "language=english\r\n").encode()
    files["reborn-key.txt"] = (key + "\r\n").encode()
    files["reborn-player.txt"] = (name + "\r\n").encode()
    return _installer_zip(out, files, f"README-{name}.txt", friend_readme(name, version))


FRIEND_FILES = config.PKG / "friend_files"


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="python -m tools.package", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--with-loader", action="store_true", help="include the Steam loader files")
    p.add_argument("--no-build", action="store_true", help="zip the DLLs already built")
    p.add_argument("--friend", metavar="NAME", help="make one friend's personal zip with a new server key")
    a = p.parse_args(argv)
    if a.friend:
        check_friend_name(a.friend)
    if not a.no_build:
        build_all()
    version = commit()
    if a.friend:
        from . import vm   # vm imports this module, so import it only here
        key = vm.new_key()
        label = a.friend.lower()
        out = make_friend_zip(DIST / "friends" / f"Reborn-{a.friend}-{version}.zip", version, a.friend, key)
        vm.KEYS_DIR.mkdir(parents=True, exist_ok=True)
        (vm.KEYS_DIR / f"reborn-key-{label}.txt").write_text(vm.key_file_text(key), encoding="utf-8", newline="")
        print(f"FRIEND ZIP OK {out}")
        print(f"KEY {label} {key}")
        print(f"On the VM:   python -m tools.vm user add {label} --key {key} [--create]   (takes effect at once, no restart)")
        print("Send the zip to that friend only; it holds the Steam loader and their key.")
        return 0
    suffix = "-with-loader" if a.with_loader else ""
    out = make_zip(DIST / f"Reborn-Playtest-{version}{suffix}.zip", version, a.with_loader)
    print(f"PACKAGE OK {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

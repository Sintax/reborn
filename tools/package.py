"""Build the playtester download: dist/Reborn-Playtest-<commit>.zip.

    python -m tools.package                # mod files, instructions, log collector
    python -m tools.package --with-loader  # also the Steam loader files from your game folder
                                               # (for a friend you send it to directly)
    python -m tools.package --friend Kaz   # everything one friend needs: loader, mod, their own
                                               # server key, player name and Steam identity, and a
                                               # guide (dist/friends/Reborn-Kaz-<commit>.zip)

The zip unpacks into the game's Binaries\\Win64 folder. Instructions: docs/PLAYTEST.md.
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
    doc = (config.REPO / "docs" / "PLAYTEST.md").read_text(encoding="utf-8")
    return f"Battleborn Reborn playtest build {version}\r\n\r\n" + doc.replace("\n", "\r\n")


def build_all() -> None:
    for project in (config.MOD_PROJECT, DXGI_PROJECT):
        cmd = build.msbuild_command()
        cmd[1] = str(project)
        p = subprocess.run(cmd, capture_output=True, text=True)
        if p.returncode != 0:
            raise RuntimeError(f"build of {project.name} failed:\n{p.stdout}{p.stderr}")


def make_zip(out: Path, version: str, with_loader: bool, win64: Path = config.WIN64,
             mod_dll: Path = config.MOD_DLL, dxgi_dll: Path = DXGI_DLL) -> Path:
    out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(dxgi_dll, "dxgi.dll")
        z.write(mod_dll, "reborn.dll")
        z.writestr("README-PLAYTEST.txt", readme(version))
        z.writestr("CollectLogs.bat", COLLECT_LOGS_BAT.replace("\n", "\r\n"))
        # Lets the player reach servers on the internet (see launch.CONFIGS_MAIN_INI); no Steam files.
        z.writestr("steam_settings/configs.main.ini", launch.CONFIGS_MAIN_INI)
        if with_loader:
            for f in (*launch.LOADER_FILES, LOADER_INI):
                if not (win64 / f).exists():
                    raise RuntimeError(f"{f} not found in {win64}")
                z.write(win64 / f, f)
    return out


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
    make_zip(out, version, True, win64, mod_dll, dxgi_dll)
    user_ini = (f"[user::general]\r\naccount_name={name}\r\naccount_steamid={friend_steamid(name)}\r\n"
                "language=english\r\n")
    with zipfile.ZipFile(out, "a", zipfile.ZIP_DEFLATED) as z:
        z.writestr("steam_settings/configs.user.ini", user_ini)
        z.writestr("reborn-key.txt", key + "\r\n")
        z.writestr("reborn-player.txt", name + "\r\n")
        z.writestr(f"README-{name}.txt", friend_readme(name, version))
    # The general playtest guide is replaced by the friend's own guide.
    _drop(out, "README-PLAYTEST.txt")
    # Their loader starts the game with the mod's console minimized: just the game window shows.
    _drop(out, LOADER_INI)
    ini = (win64 / LOADER_INI).read_text(encoding="utf-8")
    with zipfile.ZipFile(out, "a", zipfile.ZIP_DEFLATED) as z:
        z.writestr(LOADER_INI, with_min_console(ini.replace("\r\n", "\n")).replace("\n", "\r\n"))
    # Layout for the double-click installer: Install.bat/.ps1 and the guide on top, and everything
    # that belongs in the game's Win64 folder under files/ (Install.ps1 copies it there).
    _nest_under(out, "files/", keep={f"README-{name}.txt"})
    with zipfile.ZipFile(out, "a", zipfile.ZIP_DEFLATED) as z:
        for script in ("Install.bat", "Install.ps1"):
            text = (FRIEND_FILES / script).read_text(encoding="ascii").replace("\r\n", "\n")
            z.writestr(script, text.replace("\n", "\r\n"))
    return out


FRIEND_FILES = config.PKG / "friend_files"


def _nest_under(zip_path: Path, prefix: str, keep: set[str]) -> None:
    tmp = zip_path.with_name(zip_path.name + ".tmp")
    with zipfile.ZipFile(zip_path) as src, zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as dst:
        for item in src.infolist():
            name = item.filename if item.filename in keep else prefix + item.filename
            dst.writestr(name, src.read(item.filename))
    tmp.replace(zip_path)


def _drop(zip_path: Path, member: str) -> None:
    tmp = zip_path.with_name(zip_path.name + ".tmp")
    with zipfile.ZipFile(zip_path) as src, zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as dst:
        for item in src.infolist():
            if item.filename != member:
                dst.writestr(item, src.read(item.filename))
    tmp.replace(zip_path)


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
        print(f"On the VM:   python -m tools.vm addkey {label} {key}   (takes effect at once, no restart)")
        print("Send the zip to that friend only; it holds the Steam loader and their key.")
        return 0
    suffix = "-with-loader" if a.with_loader else ""
    out = make_zip(DIST / f"Reborn-Playtest-{version}{suffix}.zip", version, a.with_loader)
    print(f"PACKAGE OK {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

"""Build the playtester download: dist/Reborn-Playtest-<commit>.zip.

    python -m debugloop.package                # mod files, instructions, log collector
    python -m debugloop.package --with-loader  # also the Steam loader files from your game folder
                                               # (for a friend you send it to directly)

The zip unpacks into the game's Binaries\\Win64 folder. Instructions: docs/PLAYTEST.md.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import zipfile
from pathlib import Path

from . import build, config, launch

DIST = config.REPO / "dist"
DXGI_PROJECT = config.REPO / "dxgi" / "dxgi.vcxproj"
DXGI_DLL = config.REPO / "dxgi" / "x64" / "Release" / "dxgi.dll"   # where dxgi.vcxproj builds it
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
        if with_loader:
            for f in (*launch.LOADER_FILES, LOADER_INI):
                if not (win64 / f).exists():
                    raise RuntimeError(f"{f} not found in {win64}")
                z.write(win64 / f, f)
    return out


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="python -m debugloop.package", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--with-loader", action="store_true", help="include the Steam loader files")
    p.add_argument("--no-build", action="store_true", help="zip the DLLs already built")
    a = p.parse_args(argv)
    if not a.no_build:
        build_all()
    version = commit()
    suffix = "-with-loader" if a.with_loader else ""
    out = make_zip(DIST / f"Reborn-Playtest-{version}{suffix}.zip", version, a.with_loader)
    print(f"PACKAGE OK {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

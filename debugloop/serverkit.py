"""Build the kit for running the server on another Windows machine (a rented one, say):
dist/Reborn-ServerKit-<commit>.zip.

    python -m debugloop.serverkit             # build the mod, then zip
    python -m debugloop.serverkit --no-build  # zip the DLLs already built

The zip unpacks into that machine's Battleborn\\Binaries\\Win64 folder. It holds the Steam
loader files from your game folder, so keep it to your own machines. Instructions:
docs/SERVER-SETUP.md.
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import zipfile
import zlib
from pathlib import Path

from . import config, launch, package

FILES = config.PKG / "serverkit_files"
SCRIPTS = ("StartServer.bat", "StartServer.ps1", "StopServer.bat", "OpenFirewall.bat",
           "StartMatchmaking.bat", "StopMatchmaking.bat")
SERVER_NAME = "reborn-server"
COORDINATOR_PROJECT = config.REPO / "gamecontroller" / "gamecontroller.csproj"
COORDINATOR_PUBLISH = config.REPO / "gamecontroller" / "bin" / "serverkit-publish"


def crlf(text: str) -> str:
    return text.replace("\r\n", "\n").replace("\n", "\r\n")


def server_steamid() -> int:
    return launch.STEAMID_BASE + zlib.crc32(SERVER_NAME.encode()) % 100000


def script(name: str, version: str, files: Path = FILES) -> str:
    text = (files / name).read_text(encoding="utf-8")
    return crlf(text.replace("{{VERSION}}", version).replace("{{STEAMID}}", str(server_steamid())))


def readme(version: str) -> str:
    doc = (config.REPO / "docs" / "SERVER-SETUP.md").read_text(encoding="utf-8")
    return crlf(f"Battleborn Reborn server kit {version}\n\n" + doc)


def publish_coordinator(out: Path = COORDINATOR_PUBLISH) -> Path:
    """Publish the matchmaking service with the .NET runtime inside, for a machine without .NET."""
    if out.exists():
        shutil.rmtree(out)
    p = subprocess.run(["dotnet", "publish", str(COORDINATOR_PROJECT), "-c", "Release", "-r", "win-x64",
                        "--self-contained", "-o", str(out), "--nologo", "-v", "q"],
                       capture_output=True, text=True)
    if p.returncode != 0 or not (out / "gamecontroller.exe").exists():
        raise RuntimeError("publish of the matchmaking service failed:\n" + p.stdout + p.stderr)
    return out


def make_zip(out: Path, version: str, win64: Path = config.WIN64, mod_dll: Path = config.MOD_DLL,
             dxgi_dll: Path = package.DXGI_DLL, files: Path = FILES, matchmaking: Path | None = None) -> Path:
    for f in launch.LOADER_FILES:
        if not (win64 / f).exists():
            raise RuntimeError(f"{f} not found in {win64}")
    out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(dxgi_dll, "dxgi.dll")
        z.write(mod_dll, "reborn.dll")
        pdb = mod_dll.with_suffix(".pdb")
        if pdb.exists():   # lets a crash dump from the server name the mod's functions
            z.write(pdb, "reborn.pdb")
        for f in launch.LOADER_FILES:
            z.write(win64 / f, f)
        for name in SCRIPTS:
            z.writestr(name, script(name, version, files))
        z.writestr("CollectLogs.bat", crlf(package.COLLECT_LOGS_BAT))
        z.writestr("README-SERVER.txt", readme(version))
        if matchmaking is not None:
            for f in sorted(matchmaking.rglob("*")):
                if f.is_file():
                    z.write(f, "matchmaking/" + f.relative_to(matchmaking).as_posix())
    return out


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="python -m debugloop.serverkit", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--no-build", action="store_true", help="zip the DLLs already built")
    a = p.parse_args(argv)
    matchmaking = None
    if not a.no_build:
        package.build_all()
        matchmaking = publish_coordinator()
    version = package.commit()
    out = make_zip(package.DIST / f"Reborn-ServerKit-{version}.zip", version, matchmaking=matchmaking)
    print(f"SERVER KIT OK {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

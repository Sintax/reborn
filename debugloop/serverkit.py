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
import sys
import zipfile
import zlib
from pathlib import Path

from . import config, launch, package

FILES = config.PKG / "serverkit_files"
SCRIPTS = ("StartServer.bat", "StartServer.ps1", "StopServer.bat", "OpenFirewall.bat")
SERVER_NAME = "reborn-server"


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


def make_zip(out: Path, version: str, win64: Path = config.WIN64, mod_dll: Path = config.MOD_DLL,
             dxgi_dll: Path = package.DXGI_DLL, files: Path = FILES) -> Path:
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
    return out


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="python -m debugloop.serverkit", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--no-build", action="store_true", help="zip the DLLs already built")
    a = p.parse_args(argv)
    if not a.no_build:
        package.build_all()
    version = package.commit()
    out = make_zip(package.DIST / f"Reborn-ServerKit-{version}.zip", version)
    print(f"SERVER KIT OK {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

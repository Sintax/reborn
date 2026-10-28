"""Make the server installer (one setup exe) from a release, on the laptop:

    python -m tools.installer --version 1.0.N

It takes the release's server zip (Reborn-Server-Update-1.0.N.zip, everything under Win64/) and the
player installer zip (for the Steam loader files) from dist/private/release/assets/, adds the Visual
C++ 2012 runtime and matchmaking's default settings, and runs Inno Setup on installer/RebornServer.iss.
The result is dist/private/Reborn-Server-Setup-1.0.N.exe.

The setup exe holds the Steam loader, so it stays private: dist/private/ and the private builds
repo only. This file and the .iss are public: no server address, key, token or loader bytes in them.

Optional "free up space" parts go into the installer only when tools/trim_results.json says the
regression games passed without them, with the same file patterns tools/trim_parts.json has now.
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path, PurePosixPath

from . import config, launch, package, release, trim, version, vm

ASSETS = config.REPO / "dist" / "private" / "release" / "assets"
OUT = config.REPO / "dist" / "private"            # dist/ is gitignored; private: the loader is inside
ISS = config.REPO / "installer" / "RebornServer.iss"
TRIM_RESULTS = trim.RESULTS_FILE
TRIM_PARTS = trim.PARTS_FILE
REDIST = Path(os.environ.get("USERPROFILE", str(Path.home()))) / ".reborn" / "redist" / "vcredist_x64.exe"
REDIST_SHA256 = "681BE3E5BA9FD3DA02C09D7E565ADFA078640ED66A0D58583EFAD2C1E3CC4064"   # VC++ 2012 Update 4 x64
DEFAULT_APPSETTINGS = config.REPO / "gamecontroller" / "appsettings.json"
ISCC_MISSING = "Inno Setup is not installed. Get it from https://jrsoftware.org/isdl.php."
ISCC_PLACES = (Path(os.environ.get("LOCALAPPDATA", "")) / "Programs" / "Inno Setup 6" / "ISCC.exe",
               Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Inno Setup 6" / "ISCC.exe",
               Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Inno Setup 6" / "ISCC.exe")
ISCC_TIMEOUT_S = 1800   # the real release is ~400 MB to compress

# Steam loader files: only inside the player installer zip, under files/.
LOADER_FILES = (*launch.LOADER_FILES, package.LOADER_INI)
CONFIGS_MAIN_INI = "steam_settings/configs.main.ini"   # installed only when the server has none
# The owner's own files (relative to Win64): never shipped, never overwritten (Installer.cs ProtectedFiles).
PROTECTED = ("matchmaking/appsettings.json", "matchmaking/admin-token.txt",
             "reborn-users.json", "reborn-users-seen.json", "reborn-zips.json")
# A player's own files: the server installer never ships them either.
PLAYER_OWN = ("reborn-key.txt", "reborn-player.txt")
# Addresses a default setting may hold: "listen on every address" and this PC. Anything else could
# be a real server's address, which must never go into a public file or the default settings.
HARMLESS_IPS = {"0.0.0.0", "127.0.0.1"}
IPV4 = re.compile(r"(?<![0-9.])(?:[0-9]{1,3}\.){3}[0-9]{1,3}(?![0-9.])")


class InstallerError(RuntimeError):
    pass


def find_iscc() -> Path | None:
    """Inno Setup's command line compiler: on PATH, or where its installer puts it."""
    found = shutil.which("ISCC.exe") or shutil.which("ISCC")
    if found:
        return Path(found)
    return next((p for p in ISCC_PLACES if p.is_file()), None)


def sha256_upper(path: Path) -> str:
    return release.sha256(path).upper()


def check_redist(redist: Path, expected: str = REDIST_SHA256) -> None:
    if not Path(redist).is_file():
        raise InstallerError(f"the Visual C++ 2012 runtime installer is not at {redist}. Put the "
                             "Microsoft vcredist_x64.exe (VS 2012 Update 4) there. Nothing was built.")
    if sha256_upper(redist) != expected.upper():
        raise InstallerError(f"{redist} is not the expected Visual C++ 2012 runtime installer (its "
                             "SHA-256 does not match). Only that exact Microsoft file goes into the "
                             "installer. Nothing was built.")


def check_no_address(text: str, what: str) -> None:
    for ip in IPV4.findall(text):
        if ip not in HARMLESS_IPS:
            raise InstallerError(f"{what} holds the address {ip}; a server address must never go into "
                                 "the installer's defaults. Nothing was built.")


# ---------------------------------------------------------------- parts

def offered_parts(results_path: Path = TRIM_RESULTS, parts_path: Path = TRIM_PARTS) -> list[dict]:
    """The free-space parts the installer may offer: tested and passed, with the same file patterns
    the parts list has now. No results file (nothing tested yet) means no parts."""
    parts = trim.load_parts(parts_path)
    results = trim.load_results(results_path)
    out = []
    for p in parts:
        r = results.get(p["id"])
        if isinstance(r, dict) and r.get("passed") is True and r.get("globs") == p["globs"]:
            out.append({"id": p["id"], "label": p["label"], "globs": list(p["globs"]),
                        "bytes": int(r.get("bytes") or 0)})
    return out


def size_text(nbytes: int) -> str:
    if nbytes >= 1024 ** 3:
        return f"{nbytes / 1024 ** 3:.1f} GB"
    return f"{max(nbytes, 0) / 1024 ** 2:.0f} MB"


def _iss_string(value: str, what: str) -> str:
    if '"' in value or "\n" in value or "\r" in value:
        raise InstallerError(f"{what} {value!r} cannot go into the installer (it has a quote or a line break)")
    return f'"{value}"'


def generated_iss(ver: str, stage: Path, parts: list[dict]) -> str:
    """The include the .iss needs: the version, where the staged files are, and the offered parts
    as PartCount plus PartId<n>, PartLabel<n>, PartGlobs<n> ('|' between patterns), PartBytes<n>
    and PartSize<n> (n from 1)."""
    lines = [f"; Written by tools/installer.py for release {ver}. Not in git.",
             f"#define AppVersion {_iss_string(ver, 'the version')}",
             f"#define StageDir {_iss_string(str(stage), 'the staging folder')}",
             f"#define PartCount {len(parts)}"]
    for n, p in enumerate(parts, start=1):
        for g in p["globs"]:
            if "|" in g:
                raise InstallerError(f"the pattern {g!r} of part {p['id']} has a '|', which the installer uses between patterns")
        lines += [f"#define PartId{n} {_iss_string(p['id'], 'the part id')}",
                  f"#define PartLabel{n} {_iss_string(p['label'], 'the part label')}",
                  f"#define PartGlobs{n} {_iss_string('|'.join(p['globs']), 'the part patterns')}",
                  f"#define PartBytes{n} \"{int(p['bytes'])}\"",
                  f"#define PartSize{n} \"{size_text(int(p['bytes']))}\""]
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------- staging

def _safe_rel(name: str) -> PurePosixPath:
    rel = PurePosixPath(name)
    if rel.is_absolute() or ".." in rel.parts or ":" in name or "\\" in name:
        raise InstallerError(f"the zip holds a file with an unsafe name ({name}); nothing was built")
    return rel


def _is_kept_out(rel: PurePosixPath) -> bool:
    low = rel.as_posix().lower()
    return low in PROTECTED or rel.name.lower() in PLAYER_OWN


def stage_server_zip(server_zip: Path, ver: str, win64: Path) -> list[str]:
    """Unpack the zip's Win64/ tree into `win64`. Returns what was left out (the owner's files)."""
    left_out = []
    try:
        with zipfile.ZipFile(server_zip) as z:
            names = z.namelist()
            if "Win64/reborn.dll" not in names:
                raise InstallerError(f"{Path(server_zip).name} is not a server update zip (no Win64/reborn.dll)")
            stamped = z.read("Win64/reborn-version.txt").decode("ascii", "replace").strip() \
                if "Win64/reborn-version.txt" in names else ""
            if stamped != ver:
                raise InstallerError(f"{Path(server_zip).name} is release {stamped or 'unknown'}, not {ver}; "
                                     "nothing was built")
            for info in z.infolist():
                rel = _safe_rel(info.filename)
                if info.is_dir():
                    continue
                if rel.parts[0] != "Win64" or len(rel.parts) < 2:
                    raise InstallerError(f"{Path(server_zip).name} holds {info.filename} outside Win64/; "
                                         "nothing was built")
                inner = PurePosixPath(*rel.parts[1:])
                if _is_kept_out(inner):
                    left_out.append(inner.as_posix())
                    continue
                dest = win64.joinpath(*inner.parts)
                dest.parent.mkdir(parents=True, exist_ok=True)
                with z.open(info) as src, open(dest, "wb") as out:
                    shutil.copyfileobj(src, out)
    except (zipfile.BadZipFile, OSError) as e:
        raise InstallerError(f"could not read {server_zip}: {e}") from None
    return left_out


def stage_loader(player_zip: Path, win64: Path, loader_dir: Path) -> None:
    """The Steam loader files go next to the game; configs.main.ini goes apart (installed only if missing)."""
    try:
        with zipfile.ZipFile(player_zip) as z:
            names = set(z.namelist())
            for f in (*LOADER_FILES, CONFIGS_MAIN_INI):
                if "files/" + f not in names:
                    raise InstallerError(f"{Path(player_zip).name} has no {f}; it must be the player "
                                         "installer with the Steam loader. Nothing was built.")
            for f in LOADER_FILES:
                (win64 / f).write_bytes(z.read("files/" + f))
            dest = loader_dir.joinpath(*CONFIGS_MAIN_INI.split("/"))
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(z.read("files/" + CONFIGS_MAIN_INI))
    except (zipfile.BadZipFile, OSError) as e:
        raise InstallerError(f"could not read {player_zip}: {e}") from None


def stage(ver: str, server_zip: Path, player_zip: Path, stage_dir: Path, redist: Path,
          parts: list[dict], appsettings: Path = DEFAULT_APPSETTINGS) -> Path:
    """Lay out everything the .iss installs under stage_dir; returns the generated include."""
    win64 = stage_dir / "Win64"
    win64.mkdir(parents=True)
    stage_server_zip(server_zip, ver, win64)
    stage_loader(player_zip, win64, stage_dir / "loader")
    settings = Path(appsettings).read_bytes()
    check_no_address(settings.decode("utf-8", "replace"), f"the default matchmaking settings ({appsettings})")
    (stage_dir / "defaults").mkdir()
    (stage_dir / "defaults" / "appsettings.json").write_bytes(settings)
    (stage_dir / "Redist").mkdir()
    shutil.copyfile(redist, stage_dir / "Redist" / "vcredist_x64.exe")
    gen = stage_dir / "generated.iss"
    gen.write_text(generated_iss(ver, stage_dir, parts), encoding="utf-8")
    return gen


# ---------------------------------------------------------------- building

def setup_name(ver: str) -> str:
    return f"Reborn-Server-Setup-{ver}"


def build_installer(ver: str, server_zip: Path, player_zip: Path, out_dir: Path,
                    results: Path = TRIM_RESULTS, redist: Path = REDIST, *,
                    parts_file: Path = TRIM_PARTS, redist_sha256: str = REDIST_SHA256,
                    compression: str | None = None, iss: Path = ISS) -> Path:
    """Build out_dir/Reborn-Server-Setup-<ver>.exe. `compression` (an Inno Setup value such as
    "none") is for tests: an uncompressed setup exe can be searched byte for byte."""
    if version.parse(ver) == 0:
        raise InstallerError(f'"{ver}" is not a release number like 1.0.3')
    for what, p in (("server update zip", server_zip), ("player installer zip", player_zip)):
        if not Path(p).is_file():
            raise InstallerError(f"the {what} is not at {p}. Make the release first (python -m tools.release).")
    iscc = find_iscc()
    if iscc is None:
        raise InstallerError(ISCC_MISSING)
    check_redist(redist, redist_sha256)
    parts = offered_parts(results, parts_file)
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    exe = out_dir / (setup_name(ver) + ".exe")
    stage_dir = Path(tempfile.mkdtemp(prefix="reborn-setup-stage-"))
    try:
        gen = stage(ver, Path(server_zip), Path(player_zip), stage_dir, Path(redist), parts)
        cmd = [str(iscc), "/Q", f"/O{out_dir}", f"/F{setup_name(ver)}", f"/DGenerated={gen}"]
        if compression:
            cmd.append(f"/DCompression={compression}")
        cmd.append(str(iss))
        try:
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=ISCC_TIMEOUT_S)
        except FileNotFoundError:
            raise InstallerError(ISCC_MISSING) from None
        except subprocess.TimeoutExpired:
            raise InstallerError(f"Inno Setup did not finish within {ISCC_TIMEOUT_S // 60} minutes") from None
        if p.returncode != 0 or not exe.is_file():
            raise InstallerError(f"Inno Setup could not build the installer:\n{(p.stdout + p.stderr).strip()[-3000:]}")
    finally:
        shutil.rmtree(stage_dir, ignore_errors=True)   # the staged copy holds the loader: never left lying around
    return exe


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="python -m tools.installer", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", required=True, help="the release number, like 1.0.3")
    ap.add_argument("--assets", type=Path, default=ASSETS, help="folder with that release's zips")
    ap.add_argument("--out", type=Path, default=OUT, help="where the setup exe goes (keep it private)")
    a = ap.parse_args(argv)
    try:
        parts = offered_parts()
        exe = build_installer(a.version, a.assets / release.server_zip_name(a.version),
                              a.assets / vm.PLAYER_ZIP_ASSET, a.out)
    except (InstallerError, OSError, ValueError) as e:
        print(f"PROBLEM: {e}")
        return 1
    print(f"Free-space parts offered: {', '.join(p['label'] for p in parts) or 'none (none tested yet)'}")
    print(f"INSTALLER OK {exe}")
    print("It holds the Steam loader: keep it private (dist/private and the private builds repo only).")
    return 0


if __name__ == "__main__":
    sys.exit(main())

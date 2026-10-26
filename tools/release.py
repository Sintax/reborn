"""Make a signed, numbered release with one command, on the laptop:

    python -m tools.release --notes "What changed, in a sentence or two."

In order, it:
  1. checks the code is committed and on GitHub, and the signing key is on this PC;
  2. runs every test (Python, matchmaking, Control Panel, update installer); stops at the first failure;
  3. picks the next number 1.0.N from the releases already in Sintax/reborn-builds and builds the mod,
     dxgi, matchmaking (with .NET inside), the Control Panel, the updater and the player installer zip;
  4. makes the player update (the two DLLs plus a signed list of them) that matchmaking offers players;
  5. makes the server update zip Reborn-Server-Update-1.0.N.zip (everything under Win64/);
  6. writes update.json, which describes that zip, and signs it;
  7. uploads it all as release 1.0.N, marked latest.
The developer version header is put back at the end, whether it worked or not.

The release holds the Steam loader (inside the player installer zip), so it goes only to the private
builds repo. The files of the last release stay in dist/private/release/ for a look. The VM installs a
release with the Control Panel's Update button, or the older way: python -m tools.vm update.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

from . import config, package, serverkit, signing, version, vm

BUILDS_REPO = vm.BUILDS_REPO
OUT = config.REPO / "dist" / "private" / "release"   # dist/ is gitignored; private: the loader is inside
UPDATER_PROJECT = config.REPO / "controlpanel" / "RebornUpdater"
UPDATER_PUBLISH = UPDATER_PROJECT / "bin" / "release-publish"                    # bin/ is gitignored
UPDATER_EXE = "RebornUpdater.exe"
MATCHMAKING_PUBLISH = config.REPO / "gamecontroller" / "bin" / "release-publish"
KEY_FILES = (config.REPO / "reborn" / "UpdateKey.hpp",
             config.REPO / "controlpanel" / "Reborn.Update" / "UpdateKey.cs")
MAX_NOTES_BYTES = 4096   # the game refuses longer notes (reborn/PlayerUpdate.cpp kMaxNotesBytes)
PLAYER_DLLS = ("reborn.dll", "dxgi.dll")   # the only files a player update may hold
UPDATE_JSON = "update.json"
COMMIT_ASSET = "commit.txt"
# The owner's settings on the server: never in an update zip (the installer keeps them too).
LEFT_OUT = {name.lower() for name in (*vm.KEPT_MATCHMAKING_FILES, "reborn-users.json",
                                      "reborn-users-seen.json", "reborn-zips.json")}
SUITES = (
    ("Python tests", [sys.executable, "-m", "pytest", "debugloop/tests", "-q"]),
    ("matchmaking tests", ["dotnet", "test", "gamecontroller.Tests"]),
    ("Control Panel tests", ["dotnet", "test", "controlpanel/ControlPanel.Tests"]),
    ("update installer tests", ["dotnet", "test", "controlpanel/Reborn.Update.Tests"]),
)


class ReleaseError(RuntimeError):
    pass


def run(cmd: list[str], capture: bool = True) -> subprocess.CompletedProcess:
    """Every outside program goes through here (tests replace it). capture=False shows the output."""
    return subprocess.run(cmd, cwd=config.REPO, capture_output=capture, text=True)


def say(text: str) -> None:
    print(text, flush=True)


# --- checks ---------------------------------------------------------------------------------------

def check_notes(notes: str | None) -> str:
    notes = (notes or "").strip()
    if not notes:
        raise ReleaseError('say what changed: --notes "..." (players and the server owner see it)')
    if len(notes.encode("utf-8")) > MAX_NOTES_BYTES:
        raise ReleaseError(f"the notes are too long; keep them under {MAX_NOTES_BYTES} characters "
                           "(the game shows them in a small box)")
    return notes


def check_key(key_path: Path) -> None:
    """The key must be here, and be the one whose public half is built into the mod and the panel:
    a release signed with any other key is refused by every game and server."""
    key_path = Path(key_path)
    if not key_path.exists():
        raise ReleaseError(
            f"the signing key is not on this PC (it belongs at {key_path}). If you have a backup copy, "
            "put it back there. Only if it is lost for good, make a new one with: python -m tools.signing keygen "
            "(the new public key then has to go into " + " and ".join(f.name for f in KEY_FILES) +
            ", and players need a fresh install)")
    pub = signing.public_key_b64(key_path)
    for f in KEY_FILES:
        m = re.search(r'PublicKey\s*=\s*"([^"]+)"', f.read_text(encoding="utf-8"))
        if not m or m.group(1) != pub:
            raise ReleaseError(f"the signing key on this PC does not match the key in {f.name}; "
                               "games and the server would refuse this release")


def run_suites() -> None:
    for name, cmd in SUITES:
        say(f"  Running the {name}...")
        if run(cmd, capture=False).returncode != 0:
            shown = " ".join(["python", *cmd[1:]] if cmd[0] == sys.executable else cmd)
            raise ReleaseError(f"the {name} failed ({shown}). Nothing was built or uploaded.")


def existing_tags() -> list[str]:
    p = run(["gh", "release", "list", "--repo", BUILDS_REPO, "--limit", "1000", "--json", "tagName"])
    if p.returncode != 0:
        raise ReleaseError(f"could not read the releases in {BUILDS_REPO} (is gh signed in?): "
                           f"{(p.stderr or p.stdout).strip()[:300]}")
    try:
        return [r["tagName"] for r in json.loads(p.stdout or "[]")]
    except (ValueError, KeyError, TypeError):
        raise ReleaseError(f"could not understand the list of releases in {BUILDS_REPO}") from None


# --- building -------------------------------------------------------------------------------------

def publish_updater(out: Path = UPDATER_PUBLISH) -> Path:
    """RebornUpdater.exe as one self-contained file, published like the Control Panel."""
    if out.exists():
        shutil.rmtree(out)
    p = run(["dotnet", "publish", str(UPDATER_PROJECT), "-c", "Release", "-r", "win-x64", "--self-contained",
             "-p:PublishSingleFile=true", "-p:IncludeNativeLibrariesForSelfExtract=true",
             "-o", str(out), "--nologo", "-v", "q"])
    exe = out / UPDATER_EXE
    if p.returncode != 0 or not exe.exists():
        raise ReleaseError(f"the updater build failed:\n{p.stdout}{p.stderr}")
    return exe


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def json_bytes(data: dict) -> bytes:
    """Plain ASCII (other letters as \\u escapes) and no byte-order mark: every reader sees exactly
    the bytes that were signed."""
    return (json.dumps(data, indent=2, ensure_ascii=True) + "\n").encode("ascii")


def write_signed(path: Path, data: bytes, key_path: Path) -> None:
    sig = signing.sign(data, key_path)
    if not signing.verify(data, sig, signing.public_key_b64(key_path)):
        raise ReleaseError(f"the signature on {path.name} does not check out")
    path.write_bytes(data)
    path.with_name(path.name + ".sig").write_bytes(sig.encode("ascii"))


def make_player_update(folder: Path, ver: str, build: int, notes: str, dlls: dict[str, Path],
                       key_path: Path) -> Path:
    """The folder matchmaking serves to players: the two DLLs, manifest.json and its signature."""
    folder.mkdir(parents=True)
    files = []
    for name in PLAYER_DLLS:
        shutil.copyfile(dlls[name], folder / name)
        files.append({"name": name, "sha256": sha256(folder / name), "size": (folder / name).stat().st_size})
    manifest = {"version": ver, "build": build, "notes": notes, "files": files}
    write_signed(folder / "manifest.json", json_bytes(manifest), key_path)
    return folder


def player_update_names() -> tuple[str, ...]:
    return ("manifest.json", "manifest.json.sig", *PLAYER_DLLS)


def zip_folder(folder: Path, out: Path, names: tuple[str, ...]) -> Path:
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for name in names:
            z.write(folder / name, name)
    return out


def left_out(rel: Path) -> bool:
    return rel.name.lower() in LEFT_OUT


def make_server_zip(out: Path, ver: str, mod: dict[str, Path], player_zip: Path, matchmaking: Path,
                    player_update: Path, panel_exe: Path, updater_exe: Path) -> Path:
    """Everything under Win64/, as it lies in the server's Battleborn\\Binaries\\Win64 folder."""
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        def put(src: Path, arc: str) -> None:
            z.write(src, "Win64/" + arc)

        def text(arc: str, data: bytes) -> None:
            z.writestr("Win64/" + arc, data)

        for name, path in mod.items():
            put(path, name)
        for name in serverkit.SCRIPTS:   # the same files tools.vm update writes
            text(name, serverkit.script(name, ver).encode("utf-8"))
        text("CollectLogs.bat", serverkit.crlf(package.COLLECT_LOGS_BAT).encode("utf-8"))
        text("README-SERVER.txt", serverkit.readme(ver).encode("utf-8"))
        put(player_zip, vm.PLAYER_ZIP_ASSET)
        for f in sorted(matchmaking.rglob("*")):
            rel = f.relative_to(matchmaking)
            if f.is_file() and not left_out(rel) and rel.parts[0] != vm.PLAYER_UPDATE_DIR:
                put(f, "matchmaking/" + rel.as_posix())
        for name in player_update_names():
            put(player_update / name, f"matchmaking/{vm.PLAYER_UPDATE_DIR}/{name}")
        put(panel_exe, "ControlPanel/" + vm.PANEL_EXE)
        put(updater_exe, "ControlPanel/" + UPDATER_EXE)
        text("reborn-version.txt", (ver + "\n").encode("ascii"))
    return out


def server_zip_name(ver: str) -> str:
    return f"Reborn-Server-Update-{ver}.zip"


# --- the whole release ----------------------------------------------------------------------------

def release(notes: str, key_path: Path = signing.KEY_PATH) -> str:
    notes = check_notes(notes)
    try:
        say("Step 1 of 7: checking the code is committed and on GitHub, and the signing key is here.")
        vm.check_pushed()
        commit = vm.git("rev-parse", "HEAD")
        check_key(key_path)

        say("Step 2 of 7: running every test. This takes a few minutes.")
        run_suites()

        build = version.next_build(existing_tags())
        ver = version.version_string(build)
        say(f"Step 3 of 7: building release {ver}.")
        if OUT.exists():
            shutil.rmtree(OUT)
        assets = OUT / "assets"
        assets.mkdir(parents=True)
        package.build_all(build=build)   # the mod and dxgi, numbered
        mod = {f.name: f for f in vm.mod_files()}
        matchmaking = serverkit.publish_coordinator(MATCHMAKING_PUBLISH, self_contained=True)
        panel_exe = vm.publish_panel()
        updater_exe = publish_updater()
        player_zip = vm.build_player_zip(ver)   # after the mod build: the zip holds those DLLs

        say("Step 4 of 7: making the player update.")
        player_update = make_player_update(OUT / vm.PLAYER_UPDATE_DIR, ver, build, notes, mod, key_path)

        say("Step 5 of 7: making the server update zip.")
        zip_path = make_server_zip(assets / server_zip_name(ver), ver, mod, player_zip, matchmaking,
                                   player_update, panel_exe, updater_exe)

        say("Step 6 of 7: writing and signing update.json.")
        update = {"version": ver, "build": build, "notes": notes, "commit": commit,
                  "zip": zip_path.name, "sha256": sha256(zip_path), "size": zip_path.stat().st_size}
        write_signed(assets / UPDATE_JSON, json_bytes(update), key_path)

        # What tools.vm update downloads, under the names it looks for.
        for name in vm.MOD_FILES:
            shutil.copyfile(mod[name], assets / name)
        shutil.copyfile(panel_exe, assets / vm.PANEL_EXE)
        shutil.copyfile(player_zip, assets / vm.PLAYER_ZIP_ASSET)
        zip_folder(player_update, assets / vm.PLAYER_UPDATE_ZIP, player_update_names())
        (assets / COMMIT_ASSET).write_bytes((commit + "\n").encode("ascii"))

        say(f"Step 7 of 7: uploading release {ver} to {BUILDS_REPO}.")
        files = [UPDATE_JSON, UPDATE_JSON + ".sig", zip_path.name, *vm.MOD_FILES, vm.PANEL_EXE,
                 vm.PLAYER_ZIP_ASSET, vm.PLAYER_UPDATE_ZIP, COMMIT_ASSET]
        p = run(["gh", "release", "create", ver, *(str(assets / f) for f in files), "--repo", BUILDS_REPO,
                 "--title", ver, "--notes", notes, "--latest"])
        if p.returncode != 0:
            raise ReleaseError(
                f"the upload of {ver} did not finish: {(p.stderr or p.stdout).strip()[:300]}\n"
                f"If GitHub now shows a half-made release {ver}, remove it with "
                f"`gh release delete {ver} --repo {BUILDS_REPO} --cleanup-tag --yes`, then run this again.")
        say(f"RELEASE OK {ver}")
        say("The Control Panel on the server will offer it under Updates "
            "(or on the VM: python -m tools.vm update).")
        return ver
    finally:
        version.write_header(0)   # back to a developer build, whatever happened


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="python -m tools.release", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--notes", required=True, help="what changed, in plain words (players see it)")
    a = p.parse_args(argv)
    try:
        release(a.notes)
        return 0
    except RuntimeError as e:   # ReleaseError, VmError, and a failed build
        print(f"PROBLEM: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())

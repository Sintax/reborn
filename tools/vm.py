"""Update the rented server machine (the VM) without copying the whole server kit.

On the laptop, after pushing the commit:

    python -m tools.vm release              # build the mod, upload it as release <commit>
    python -m tools.vm release --no-build   # upload the DLLs already built

On the VM, from its clone of this repo (Claude there runs it):

    python -m tools.vm update               # newest release
    python -m tools.vm update --version abc1234 --no-start
    python -m tools.vm update --version 1.0.4  # a numbered release from `python -m tools.release`

A numbered release (1.0.N) says which commit it was built from in its commit.txt; the VM checks out
that commit. It also carries the player update (player-update.zip), which goes into
matchmaking\\player-update so matchmaking can offer it to players' games, and the Control Panel's
updater (RebornUpdater.exe), which goes beside the panel. Install the first numbered release this
way; after that use the panel's Update now (mixing the two leaves gaps in the panel's saved versions).

Users (who may join the server, and who may start games). The list is reborn-users.json in the
game's Win64 folder; changes take effect within a second, no restart:

    python -m tools.vm key mike                          # laptop: make a key, write dist/keys/reborn-key-mike.txt
    python -m tools.vm user add mike --key <key>         # VM: add them (--create lets them start games)
    python -m tools.vm user add mike --create            # VM: or let it make the key (printed once)
    python -m tools.vm user list                         # VM: who is on the list (never shows keys)
    python -m tools.vm user rename mike Mike2            # VM: change a name
    python -m tools.vm user create mike on|off           # VM: may they start games?
    python -m tools.vm user revoke mike                  # VM: remove them
    python -m tools.vm user require-keys on|off          # VM: off = anyone may join

Only the mod (reborn.dll, reborn.pdb, dxgi.dll, about 11 MB), the Control Panel (ControlPanel.exe,
about 130 MB, one file) and the player installer zip (reborn-player-installer.zip) travel, through
the private repo Sintax/reborn-builds. That zip is the installer WITH the Steam loader, the same
start as a `python -m tools.package --friend` zip: the panel adds one friend's name and key to it.
So it goes only to the private builds repo and the VM; it is built in dist/private/, never under
a public zip's name, and the loader files are never committed. The update puts the panel in a
ControlPanel folder in Win64 and the zip in Win64 itself, and makes a "Reborn Control Panel"
desktop shortcut once; close the panel before updating. The user list, the "last seen" list, the
friend-zip record and the matchmaking token are never touched. The VM checks out the same commit
from GitHub, so its scripts and matchmaking service match the mod; it builds the service itself
with its own .NET. Instructions: docs/TWO-MACHINES.md.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import secrets
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request
import zipfile
from datetime import datetime, timezone
from pathlib import Path

from . import config, package
from . import version as release_numbers   # `version` is a parameter name all through this file

BUILDS_REPO = "Sintax/reborn-builds"
MOD_FILES = ("reborn.dll", "reborn.pdb", "dxgi.dll")
PANEL_EXE = "ControlPanel.exe"                     # a release asset, and the installed file's name
PLAYER_ZIP_ASSET = "reborn-player-installer.zip"   # the player installer under the name the panel looks for
PLAYER_UPDATE_ZIP = "player-update.zip"            # numbered releases: what matchmaking offers players
PLAYER_UPDATE_DIR = "player-update"                # ...unpacked into matchmaking\player-update
PLAYER_UPDATE_FILES = ("manifest.json", "manifest.json.sig", "reborn.dll", "dxgi.dll")
COMMIT_ASSET = "commit.txt"                        # numbered releases: the commit they were built from
COMMIT_RE = re.compile(r"[0-9a-f]{7,40}")
UPDATER_EXE = "RebornUpdater.exe"                  # numbered releases: the panel's updater, beside the panel
# A numbered release also holds the big server update zip, which this update does not use.
VM_ASSETS = (*MOD_FILES, PANEL_EXE, UPDATER_EXE, PLAYER_ZIP_ASSET, PLAYER_UPDATE_ZIP)
# A release number as the mod carries it (Version.hpp's text, a C string): "1.0.7\0".
NUMBERED_DLL_RE = re.compile(rb"(?<![0-9.])1\.0\.[1-9][0-9]*\x00")
GH_MISSING = ("GitHub's command line tool (gh) is not installed. Install it from https://cli.github.com "
              "and sign in with gh auth login.")
PANEL_PROJECT = config.REPO / "controlpanel" / "ControlPanel"
PANEL_PUBLISH = PANEL_PROJECT / "bin" / "release-publish"                  # bin/ is gitignored
# The player installer WITH the Steam loader (dist/ is gitignored). Private: it goes only to the
# private builds repo, so it is kept apart from any public zip and named so it is never taken for one.
PLAYER_ZIP = config.REPO / "dist" / "private" / "reborn-player-installer-with-loader.zip"
SHORTCUT_NAME = "Reborn Control Panel"
SHORTCUT_TIMEOUT_SECONDS = 30
KEPT_MATCHMAKING_FILES = ("appsettings.json", "admin-token.txt")   # the owner's roster, and the panel's key
PANEL_OPEN_MESSAGE = "Close the Control Panel and run the update again."
DEPLOYED_MARK = "reborn-deployed.txt"
GAMES_URL = "http://localhost:5000/api/games"
KEY_ALPHABET = "ABCDEFGHJKMNPQRSTVWXYZ23456789"   # no 0/O/1/I/L: friends may type it
KEY_LENGTH = 24
KEYS_DIR = config.REPO / "dist" / "keys"          # dist/ is gitignored
USERS_FILE = "reborn-users.json"
NAME_RE = re.compile(r"[A-Za-z0-9_-]{1,24}")


class VmError(RuntimeError):
    pass


def _run(cmd: list[str], cwd: Path) -> subprocess.CompletedProcess:
    try:
        return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    except FileNotFoundError:
        if cmd[0] == "gh":
            raise VmError(GH_MISSING) from None
        raise


def run(cmd: list[str], cwd: Path = config.REPO, check: bool = True) -> str:
    p = _run(cmd, cwd)
    if check and p.returncode != 0:
        raise VmError(f"{' '.join(cmd)} failed:\n{p.stdout}{p.stderr}")
    return p.stdout.strip()


def succeeds(cmd: list[str], cwd: Path = config.REPO) -> bool:
    """True when the program ran and said yes (exit code 0)."""
    return _run(cmd, cwd).returncode == 0


def git(*args: str, check: bool = True) -> str:
    return run(["git", *args], check=check)


# --- laptop ---------------------------------------------------------------------------------

def mod_files() -> list[Path]:
    files = [config.MOD_DLL, config.MOD_DLL.with_suffix(".pdb"), package.DXGI_DLL]
    for f in files:
        if not f.exists():
            raise VmError(f"{f} is missing; build first (drop --no-build)")
    return files


def check_pushed() -> str:
    if git("status", "--porcelain", "--untracked-files=no"):
        raise VmError("there are uncommitted changes; commit them so the VM gets the same code")
    if not git("branch", "-r", "--contains", "HEAD"):
        raise VmError("this commit is not on GitHub yet; push it first so the VM can check it out")
    return git("rev-parse", "--short", "HEAD")


def publish_panel(out: Path = PANEL_PUBLISH) -> Path:
    """The Control Panel as one self-contained exe (no .NET needed on the VM). The native WPF files
    are packed inside it: published without that, the exe alone crashes when it starts."""
    if out.exists():
        shutil.rmtree(out)
    run(["dotnet", "publish", str(PANEL_PROJECT), "-c", "Release", "-r", "win-x64", "--self-contained",
         "-p:PublishSingleFile=true", "-p:IncludeNativeLibrariesForSelfExtract=true",
         "-o", str(out), "--nologo", "-v", "q"])
    exe = out / PANEL_EXE
    if not exe.exists():
        raise VmError(f"the Control Panel build made no {PANEL_EXE}")
    return exe


def build_player_zip(version: str) -> Path:
    """The player installer with the Steam loader, built from the DLLs just built and the loader
    files in this PC's game folder: what a `python -m tools.package --friend` zip starts from. The
    panel adds one friend's name, key and Steam identity to it, so a friend's zip has everything to
    play. It holds the loader, so it travels only through the private builds repo."""
    return package.make_zip(PLAYER_ZIP, version, with_loader=True)


def panel_files() -> list[Path]:
    exe = PANEL_PUBLISH / PANEL_EXE
    if not exe.exists():
        raise VmError(f"{exe} is missing; build first (drop --no-build)")
    if not PLAYER_ZIP.exists():
        raise VmError(f"{PLAYER_ZIP} is missing; run the release again without --no-build to make it")
    return [exe, PLAYER_ZIP]


def check_not_numbered(files: list[Path]) -> None:
    """The reverse of tools.release's check: a release named after a commit must hold a developer
    build. After `python -m tools.release` the DLLs on disk call themselves 1.0.N until rebuilt, and
    uploading them here would put a numbered mod under a commit's name."""
    for f in files:
        if f.name not in ("reborn.dll", "dxgi.dll"):
            continue
        m = NUMBERED_DLL_RE.search(f.read_bytes())
        if m:
            number = m.group()[:-1].decode("ascii")
            raise VmError(f"{f.name} on disk is release {number}, left from python -m tools.release. "
                          "Build it again first (run this without --no-build, or python -m tools.build). "
                          "Nothing was uploaded.")


def release(build: bool) -> str:
    version = check_pushed()
    if build:
        package.build_all()
        publish_panel()
        build_player_zip(version)   # after the mod build: the zip holds those DLLs
    mod_paths = mod_files()
    check_not_numbered(mod_paths)
    mod = [str(f) for f in mod_paths]
    panel_exe, player_zip = panel_files()   # all checked before anything is uploaded
    notes = f"{git('rev-parse', '--abbrev-ref', 'HEAD')}: {git('log', '-1', '--format=%s')}"
    exists = succeeds(["gh", "release", "view", version, "--repo", BUILDS_REPO])
    # Once `python -m tools.release` has made numbered releases, the newest of those must stay
    # "latest": the Control Panel and a plain `tools.vm update` look there for update.json and the
    # player update, and this kind of release has neither.
    latest = not numbered_releases_exist()
    with tempfile.TemporaryDirectory() as stage:
        # the asset takes the file's name, and the built zip has a different one
        zip_asset = Path(stage) / PLAYER_ZIP_ASSET
        shutil.copyfile(player_zip, zip_asset)
        files = [*mod, str(panel_exe), str(zip_asset)]
        if exists:
            run(["gh", "release", "upload", version, *files, "--repo", BUILDS_REPO, "--clobber"])
        else:
            run(["gh", "release", "create", version, *files, "--repo", BUILDS_REPO,
                 "--title", version, "--notes", notes, "--latest" if latest else "--latest=false"])
    if not latest and not exists:
        print("Numbered releases exist; this one is not marked latest. "
              f"Install it on the VM with: python -m tools.vm update --version {version}")
    return version


def numbered_releases_exist() -> bool:
    out = run(["gh", "release", "list", "--repo", BUILDS_REPO, "--limit", "1000", "--json", "tagName"])
    try:
        tags = [r["tagName"] for r in json.loads(out or "[]")]
    except (ValueError, KeyError, TypeError):
        raise VmError(f"could not understand the list of releases in {BUILDS_REPO}") from None
    return any(release_numbers.parse(t) > 0 for t in tags)


# --- VM -------------------------------------------------------------------------------------

def latest_version() -> str:
    out = run(["gh", "release", "view", "--repo", BUILDS_REPO, "--json", "tagName"])
    return json.loads(out)["tagName"]


def release_assets(version: str) -> set[str]:
    out = run(["gh", "release", "view", version, "--repo", BUILDS_REPO, "--json", "assets"])
    try:
        return {a["name"] for a in json.loads(out)["assets"]}
    except (ValueError, KeyError, TypeError):
        raise VmError(f"could not understand the file list of release {version}") from None


def check_latest_keeps_player_update(version: str, win64: Path) -> None:
    """A plain `update` (no --version) must never quietly take away the player update that
    matchmaking already offers: installing matchmaking empties its folder, and a release without
    player-update.zip has nothing to put back. Naming the release with --version does it on purpose."""
    if not (win64 / "matchmaking" / PLAYER_UPDATE_DIR).exists():
        return
    if PLAYER_UPDATE_ZIP not in release_assets(version):
        raise VmError(f"the newest release ({version}) has no player update, so installing it would stop "
                      "players' games being offered the new mod. Nothing was changed. Install a numbered "
                      "release (python -m tools.vm update --version 1.0.N), or, to install this one anyway, "
                      f"python -m tools.vm update --version {version}")


def release_commit(version: str) -> str:
    """The commit to check out for a release. Older releases are named after their commit; a
    numbered one (1.0.N, from tools.release) names it in its commit.txt."""
    if release_numbers.parse(version) == 0:
        return version
    with tempfile.TemporaryDirectory() as d:
        run(["gh", "release", "download", version, "--repo", BUILDS_REPO, "--pattern", COMMIT_ASSET,
             "--dir", d])
        f = Path(d) / COMMIT_ASSET
        sha = f.read_text(encoding="utf-8", errors="replace").strip() if f.exists() else ""
    if not COMMIT_RE.fullmatch(sha):
        raise VmError(f"release {version} does not say which commit it was built from; nothing was changed")
    return sha


def checkout(version: str) -> None:
    if git("status", "--porcelain", "--untracked-files=no"):
        raise VmError(f"the clone at {config.REPO} has local edits; the VM should not be edited by hand")
    git("fetch", "origin", "--quiet")
    git("checkout", "--detach", "--quiet", version)


def stop_servers() -> None:
    for exe in ("gamecontroller.exe", "Serverborn.exe"):
        run(["taskkill", "/im", exe, "/f"], check=False)
    deadline = time.time() + 20
    while time.time() < deadline and "Serverborn.exe" in run(["tasklist"], check=False):
        time.sleep(0.5)


def download_mod(version: str, scratch: Path) -> Path:
    if scratch.exists():
        shutil.rmtree(scratch)
    cmd = ["gh", "release", "download", version, "--repo", BUILDS_REPO, "--dir", str(scratch)]
    if release_numbers.parse(version) > 0:   # a numbered release: leave out the big server update zip
        for name in VM_ASSETS:
            cmd += ["--pattern", name]
    run(cmd)
    for name in MOD_FILES:   # the panel and the zip may be missing: older releases (rollback) have none
        if not (scratch / name).exists():
            raise VmError(f"release {version} has no {name}")
    return scratch


def install_mod(downloaded: Path, win64: Path) -> None:
    for name in MOD_FILES:
        shutil.copy2(downloaded / name, win64 / name)


def stage_panel(downloaded: Path, win64: Path) -> list[tuple[str, Path, Path]]:
    """First half of installing the Control Panel and the player installer zip: copy them beside
    where they go, as <name>.new. This is the slow part (the exe is about 128 MB) and the part that
    can fail on a full disk, so it runs before anything is stopped. The panel goes in its own
    folder; the zip it copies for friends sits beside the game files, where the panel looks for it.
    A release without one of them (an older release, when rolling back) leaves the installed one.
    Returns (what it is, the .new copy, where it goes). On a failure the copies are removed again
    and the error is a VmError."""
    wanted = [("Control Panel", PANEL_EXE, win64 / "ControlPanel" / PANEL_EXE,
               "This release has no Control Panel; keeping the one already installed."),
              # The panel's Update now and Roll back start this one; it must sit beside the panel.
              ("Control Panel's updater", UPDATER_EXE, win64 / "ControlPanel" / UPDATER_EXE,
               f"This release has no updater ({UPDATER_EXE}); keeping the one already installed, if any."),
              ("player installer zip", PLAYER_ZIP_ASSET, win64 / PLAYER_ZIP_ASSET,
               "This release has no player installer zip; keeping the one already installed.")]
    staged: list[tuple[str, Path, Path]] = []
    try:
        for label, name, dest, missing in wanted:
            if not (downloaded / name).exists():
                print(missing)
                continue
            dest.parent.mkdir(parents=True, exist_ok=True)
            tmp = dest.with_name(dest.name + ".new")
            staged.append((label, tmp, dest))
            shutil.copy2(downloaded / name, tmp)
    except OSError as e:
        discard_staged(staged)
        raise VmError(f"the {label} could not be copied ({e}); nothing was changed") from None
    return staged


def discard_staged(staged: list[tuple[str, Path, Path]]) -> None:
    for _, tmp, _ in staged:
        try:
            tmp.unlink(missing_ok=True)
        except OSError:
            pass


def swap_panel(staged: list[tuple[str, Path, Path]]) -> None:
    """Second half: one rename per file, so it is never half-copied. A rename that fails (say a
    virus scanner holds the file) keeps the old file, says so, and does not fail the update."""
    for label, tmp, dest in staged:
        try:
            os.replace(tmp, dest)
        except OSError as e:
            print(f"The {label} could not be updated: {e}; the old one is kept.")
            discard_staged([(label, tmp, dest)])


def install_panel(downloaded: Path, win64: Path) -> None:
    swap_panel(stage_panel(downloaded, win64))


def panel_running() -> bool:
    """A running exe is locked: it cannot be replaced until the owner closes the panel."""
    out = run(["tasklist", "/fi", f"imagename eq {PANEL_EXE}", "/nh"], check=False)
    return PANEL_EXE.lower() in out.lower()


def ensure_panel_closed() -> None:
    if panel_running():
        raise VmError(PANEL_OPEN_MESSAGE)


SHORTCUT_SCRIPT = r"""
$ErrorActionPreference = 'Stop'
$dir = $env:RB_SHORTCUT_DIR
if (-not $dir) { $dir = [Environment]::GetFolderPath('Desktop') }
$link = Join-Path $dir ($env:RB_SHORTCUT_NAME + '.lnk')
if (Test-Path -LiteralPath $link) { 'EXISTS'; exit 0 }
$s = (New-Object -ComObject WScript.Shell).CreateShortcut($link)
$s.TargetPath = $env:RB_SHORTCUT_TARGET
$s.WorkingDirectory = $env:RB_SHORTCUT_WORKDIR
$s.Description = 'Battleborn Reborn Control Panel'
$s.Save()
'CREATED'
"""


def ensure_desktop_shortcut(win64: Path, desktop: Path | None = None) -> bool:
    """Make the 'Reborn Control Panel' shortcut on the desktop of whoever runs this, if it is not
    there yet (one the owner moved or renamed is never touched, one they deleted comes back).
    True when made, False when it was already there. Values go in through the environment so no
    path needs quoting. `desktop` is for tests."""
    exe = win64 / "ControlPanel" / PANEL_EXE
    env = {**os.environ, "RB_SHORTCUT_NAME": SHORTCUT_NAME, "RB_SHORTCUT_TARGET": str(exe),
           "RB_SHORTCUT_WORKDIR": str(exe.parent)}
    if desktop is not None:
        env["RB_SHORTCUT_DIR"] = str(desktop)
    try:
        p = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                            "-Command", SHORTCUT_SCRIPT], env=env, capture_output=True, text=True,
                           timeout=SHORTCUT_TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        raise VmError(f"could not make the desktop shortcut: PowerShell did not answer "
                      f"in {SHORTCUT_TIMEOUT_SECONDS} seconds") from None
    except OSError as e:
        raise VmError(f"could not make the desktop shortcut: PowerShell would not start ({e})") from None
    answer = p.stdout.strip().splitlines()[-1:] or [""]
    if p.returncode != 0 or answer[0] not in ("CREATED", "EXISTS"):
        raise VmError(f"could not make the desktop shortcut: {(p.stderr or p.stdout).strip()[:200]}")
    return answer[0] == "CREATED"


def install_scripts(version: str, win64: Path) -> None:
    from . import serverkit
    for name in serverkit.SCRIPTS:
        (win64 / name).write_bytes(serverkit.script(name, version).encode("utf-8"))
    (win64 / "CollectLogs.bat").write_bytes(serverkit.crlf(package.COLLECT_LOGS_BAT).encode("utf-8"))
    (win64 / "README-SERVER.txt").write_bytes(serverkit.readme(version).encode("utf-8"))


def build_matchmaking(scratch: Path) -> Path:
    """Build the service with this machine's .NET (small; the runtime is not inside)."""
    from . import serverkit
    return serverkit.publish_coordinator(scratch, self_contained=False)


def install_matchmaking(published: Path, win64: Path) -> None:
    """The owner's appsettings.json (which games run) and the panel's admin-token.txt are kept, byte
    for byte; a first install gets the repo's settings and matchmaking makes its own token."""
    dest = win64 / "matchmaking"
    kept = {name: (dest / name).read_bytes() for name in KEPT_MATCHMAKING_FILES if (dest / name).exists()}
    if dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(published, dest)
    for name, data in kept.items():
        (dest / name).write_bytes(data)


def stage_player_update(downloaded: Path, scratch: Path) -> Path | None:
    """Unpack the release's player update into scratch and check it holds exactly the four files
    matchmaking serves, before anything is stopped. None when the release has none (older ones)."""
    src = downloaded / PLAYER_UPDATE_ZIP
    if not src.exists():
        print("This release has no player update; players' games will not be offered one.")
        return None
    if scratch.exists():
        shutil.rmtree(scratch)
    scratch.mkdir(parents=True)
    try:
        with zipfile.ZipFile(src) as z:
            if sorted(z.namelist()) != sorted(PLAYER_UPDATE_FILES):
                raise VmError("the player update in this release does not hold the expected files; "
                              "nothing was changed")
            for name in PLAYER_UPDATE_FILES:
                (scratch / name).write_bytes(z.read(name))
    except (zipfile.BadZipFile, OSError) as e:
        raise VmError(f"the player update in this release cannot be read ({e}); nothing was changed") from None
    return scratch


def install_player_update(staged: Path | None, win64: Path) -> None:
    """Put the player update in matchmaking\\player-update. Run after install_matchmaking, which
    empties the matchmaking folder. The folder is filled beside its place and then renamed in, so
    matchmaking never serves a new list with an old signature. A failure here does not fail the
    update: the server runs, players are just not offered the new mod yet."""
    if staged is None:
        return
    dest = win64 / "matchmaking" / PLAYER_UPDATE_DIR
    tmp = dest.with_name(dest.name + ".new")
    try:
        if tmp.exists():
            shutil.rmtree(tmp)
        shutil.copytree(staged, tmp)
        if dest.exists():
            shutil.rmtree(dest)
        os.replace(tmp, dest)
    except OSError as e:
        print(f"The player update could not be put in place ({e}); players' games will not be "
              "offered the new mod until the next update.")
        shutil.rmtree(tmp, ignore_errors=True)


def start_matchmaking(win64: Path) -> None:
    subprocess.Popen(["cmd", "/c", str(win64 / "StartMatchmaking.bat")], cwd=win64,
                     creationflags=subprocess.CREATE_NEW_CONSOLE | subprocess.CREATE_NEW_PROCESS_GROUP)


def ready_games(games) -> list[dict]:
    """The games whose "ready" is true (any letter case). Matchmaking lists a game the moment it
    starts the game server, with "ready": false until that server has loaded and checked in, so only
    a ready game shows the new version works."""
    if not isinstance(games, list):
        return []
    return [g for g in games if isinstance(g, dict)
            and any(isinstance(k, str) and k.lower() == "ready" and v is True for k, v in g.items())]


def wait_for_game(seconds: int = 180) -> list[dict]:
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            with urllib.request.urlopen(GAMES_URL, timeout=5) as r:
                games = ready_games(json.load(r))
            if games:
                return games
        except (OSError, ValueError):   # not answering yet, or not JSON
            pass
        time.sleep(5)
    return []


def newest_log_says_listening() -> bool:
    folders = sorted((d for d in config.LOG_DIR.glob("rb-*") if d.is_dir()),
                     key=lambda d: d.stat().st_mtime, reverse=True)
    for log in (folders[0].glob("*.log") if folders else []):
        if "Game networking listening on port" in log.read_text(encoding="utf-8", errors="replace"):
            return True
    return False


def firewall_rules_missing() -> list[str]:
    missing = []
    for rule in ("Battleborn Reborn server", "Battleborn Reborn matchmaking"):
        p = subprocess.run(["netsh", "advfirewall", "firewall", "show", "rule", f"name={rule}"],
                           capture_output=True, text=True)
        if p.returncode != 0:
            missing.append(rule)
    return missing


def update(version: str | None, win64: Path, start: bool, checked_out: bool) -> int:
    if not (win64 / "Battleborn.exe").exists():
        raise VmError(f"Battleborn.exe is not in {win64}; set BB_GAME_DIR to the game folder")
    ensure_panel_closed()   # first of all: an open panel locks its exe, and nothing must be half-done
    if not version:
        version = latest_version()
        check_latest_keeps_player_update(version, win64)
    if not checked_out:
        checkout(release_commit(version))
        # Carry on with the code of the version just checked out, not the code that started.
        return subprocess.call([sys.executable, "-m", "tools.vm", "update", "--version", version,
                                "--win64", str(win64), "--checked-out", *([] if start else ["--no-start"])],
                               cwd=config.REPO)
    print(f"Updating the server to {version}")
    scratch = Path(os.environ.get("TEMP", str(config.REPO))) / "reborn-vm-update"
    downloaded = download_mod(version, scratch / "mod")      # both before stopping anything,
    published = build_matchmaking(scratch / "matchmaking")  # so a failure leaves the server up
    player_update = stage_player_update(downloaded, scratch / PLAYER_UPDATE_DIR)
    ensure_panel_closed()   # again: the downloads take a while, and the panel may have been opened since
    staged = stage_panel(downloaded, win64)   # the big copy, while the old server still runs
    stop_servers()
    install_mod(downloaded, win64)
    install_scripts(version, win64)
    install_matchmaking(published, win64)
    install_player_update(player_update, win64)   # after: install_matchmaking empties that folder
    swap_panel(staged)
    (win64 / DEPLOYED_MARK).write_text(version + "\n", encoding="utf-8")
    print(f"Installed {version} into {win64}")
    try:
        if (win64 / "ControlPanel" / PANEL_EXE).exists() and ensure_desktop_shortcut(win64):
            print(f"Made the '{SHORTCUT_NAME}' shortcut on the desktop")
    except (VmError, OSError) as e:   # a missing shortcut is not worth a failed update
        print(f"NOTE: {e}; open {win64 / 'ControlPanel' / PANEL_EXE} instead")
    for rule in firewall_rules_missing():
        print(f"WARNING: Windows Firewall rule '{rule}' is missing; run OpenFirewall.bat once")
    if not start:
        print("UPDATE OK (not started)")
        return 0
    start_matchmaking(win64)
    games = wait_for_game()
    if not games:
        log = win64 / "matchmaking" / "matchmaking.log"
        print(f"PROBLEM: no game server was ready at {GAMES_URL} after 3 minutes; read {log} "
              f"and the newest rb-* folder in {config.LOG_DIR}")
        if log.exists():
            print("Last lines of the matchmaking log:")
            print("\n".join(log.read_text(encoding="utf-8", errors="replace").splitlines()[-20:]))
        return 1
    for g in games:
        print(f"Game ready: {g.get('instanceName')}  {g.get('serverConnectString')}")
    deadline = time.time() + 90   # ready; the server log should also say it is listening
    while not newest_log_says_listening() and time.time() < deadline:
        time.sleep(5)
    if not newest_log_says_listening():
        print("NOTE: after 90 s the newest server log still does not say it is listening; check it")
    print("UPDATE OK")
    return 0


# --- users: who may join, and who may start games (gamecontroller UserList.cs) ----------------

def new_key(rng=secrets) -> str:
    return "".join(rng.choice(KEY_ALPHABET) for _ in range(KEY_LENGTH))


def key_file_text(key: str) -> str:
    return key + "\r\n"


def valid_name(name: str) -> bool:
    """The mod's player-name rule (PlayerNameRules.hpp CleanName): 1-24 of A-Z a-z 0-9 _ -."""
    return isinstance(name, str) and NAME_RE.fullmatch(name) is not None


def valid_key(key: str) -> bool:
    return isinstance(key, str) and len(key) == KEY_LENGTH and all(c in KEY_ALPHABET for c in key)


def default_users_path() -> Path:
    return config.WIN64 / USERS_FILE


def load_users(path: Path) -> dict:
    """The user list as written, or an empty one (keys required) when there is no file yet.
    A file that cannot be read is an error: it is never overwritten, so nothing is lost."""
    if not path.exists():
        return {"RequireKeys": True, "Users": []}
    try:
        data = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, ValueError) as e:
        raise VmError(f"{path} cannot be read ({type(e).__name__}); fix or remove it first") from None
    if not is_user_list(data):
        raise VmError(f"{path} is not a user list (expected RequireKeys and Users); fix or remove it first")
    data.setdefault("RequireKeys", True)
    return data


def is_user_list(data) -> bool:
    """The one strict shape every reader accepts (gamecontroller UserList.cs, reborn/AccessRules.hpp):
    Users is a list of objects whose Name and Key are non-empty strings; RequireKeys, CanCreateGames
    and ConfirmName may be left out but are true/false when present. Anything else is unreadable."""
    def optional_bool(obj: dict, name: str) -> bool:
        return name not in obj or isinstance(obj[name], bool)

    def good_user(u) -> bool:
        return (isinstance(u, dict)
                and isinstance(u.get("Name"), str) and u["Name"] != ""
                and isinstance(u.get("Key"), str) and u["Key"] != ""
                and optional_bool(u, "CanCreateGames") and optional_bool(u, "ConfirmName"))

    return (isinstance(data, dict) and optional_bool(data, "RequireKeys")
            and isinstance(data.get("Users"), list) and all(good_user(u) for u in data["Users"]))


def save_users(path: Path, data: dict) -> None:
    """Every write goes through here. The server re-reads this file whenever it changes, so it
    must never see a half-written file: write a sibling, then swap it in with one rename."""
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    os.replace(tmp, path)


def _find_user(data: dict, name: str) -> dict | None:
    return next((u for u in data["Users"] if u["Name"].lower() == name.lower()), None)


def _need_user(data: dict, name: str) -> dict:
    user = _find_user(data, name)
    if user is None:
        raise VmError(f"there is no user called {name}")
    return user


def user_add(path: Path, name: str, can_create: bool = False, key: str | None = None) -> str:
    if not valid_name(name):
        raise VmError(f"{name!r} is not a usable name: 1 to 24 letters, digits, _ or -")
    if key is not None and not valid_key(key):
        raise VmError(f"that key is not valid: it must be {KEY_LENGTH} characters from {KEY_ALPHABET}")
    data = load_users(path)
    if _find_user(data, name) is not None:
        raise VmError(f"{name} is already a user (names ignore upper and lower case)")
    if key is None:
        key = new_key()
    elif any(u.get("Key") == key for u in data["Users"]):
        raise VmError("that key already belongs to another user")
    data["Users"].append({"Name": name, "Key": key, "CanCreateGames": can_create, "ConfirmName": False,
                          "Added": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")})
    save_users(path, data)
    return key


def user_rename(path: Path, old: str, new: str) -> None:
    if not valid_name(new):
        raise VmError(f"{new!r} is not a usable name: 1 to 24 letters, digits, _ or -")
    data = load_users(path)
    user = _need_user(data, old)
    other = _find_user(data, new)
    if other is not None and other is not user:
        raise VmError(f"{new} is already another user (names ignore upper and lower case)")
    user["Name"] = new
    user["ConfirmName"] = False   # the player's own name must now match what they join with
    save_users(path, data)


def user_set_create(path: Path, name: str, can_create: bool) -> None:
    data = load_users(path)
    _need_user(data, name)["CanCreateGames"] = can_create
    save_users(path, data)


def user_revoke(path: Path, name: str) -> None:
    data = load_users(path)
    data["Users"].remove(_need_user(data, name))
    save_users(path, data)


def user_set_require_keys(path: Path, on: bool) -> None:
    data = load_users(path)
    data["RequireKeys"] = on
    save_users(path, data)


def user_list_lines(path: Path) -> list[str]:
    """One line per user, never the key."""
    if not path.exists():
        return [f"no user list yet ({path}): anyone can join, nobody can start games"]
    data = load_users(path)
    yn = lambda b: "yes" if b else "no"
    lines = [f"{u['Name']}  create={yn(u.get('CanCreateGames') is True)}  confirm-name={yn(u.get('ConfirmName') is True)}"
             f"  added={str(u.get('Added') or '?')[:10]}" for u in data["Users"]]
    if not lines:
        lines.append("(no users)")
    lines.append(f"require keys: {'on' if data['RequireKeys'] is not False else 'off'}")
    return lines


def make_key(label: str) -> Path:
    if not valid_name(label):
        raise VmError(f"{label!r} is not a usable name: 1 to 24 letters, digits, _ or -")
    key = new_key()
    KEYS_DIR.mkdir(parents=True, exist_ok=True)
    out = KEYS_DIR / f"reborn-key-{label}.txt"
    out.write_text(key_file_text(key), encoding="utf-8", newline="")
    print(f"KEY {label} {key}")
    print(f"Friend file: {out}  (they rename it to reborn-key.txt in their game's Binaries\\Win64 folder)")
    print(f"On the VM:   python -m tools.vm user add {label} --key {key} [--create]   (takes effect at once, no restart)")
    return out


def _run_user(a: argparse.Namespace) -> int:
    path = a.users or default_users_path()
    if a.user_cmd == "list":
        print("\n".join(user_list_lines(path)))
    elif a.user_cmd == "add":
        key = user_add(path, a.name, a.create, a.key)
        print(f"KEY {a.name} {key}")
        print(f"USER ADDED {a.name}{' (can create games)' if a.create else ''}; takes effect at once, no restart")
    elif a.user_cmd == "rename":
        user_rename(path, a.old, a.new)
        print(f"USER RENAMED {a.old} -> {a.new}")
    elif a.user_cmd == "create":
        user_set_create(path, a.name, a.state == "on")
        print(f"USER {a.name}: can create games {a.state}")
    elif a.user_cmd == "revoke":
        user_revoke(path, a.name)
        print(f"USER REVOKED {a.name}")
    else:
        user_set_require_keys(path, a.state == "on")
        print(f"REQUIRE KEYS {a.state}")
    return 0


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="python -m tools.vm", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("release", help="laptop: upload the built mod for the VM")
    r.add_argument("--no-build", action="store_true", help="upload the DLLs already built")
    u = sub.add_parser("update", help="VM: install a release and restart matchmaking")
    u.add_argument("--version", help="release to install (default: newest)")
    u.add_argument("--win64", type=Path, default=config.WIN64)
    u.add_argument("--no-start", action="store_true", help="install only, start nothing")
    u.add_argument("--checked-out", action="store_true", help=argparse.SUPPRESS)
    k = sub.add_parser("key", help="laptop: make a key and the file to send a friend")
    k.add_argument("label", help="who it is for, e.g. mike")
    us = sub.add_parser("user", help="VM: who may join, and who may start games")
    usub = us.add_subparsers(dest="user_cmd", required=True)
    where = argparse.ArgumentParser(add_help=False)
    where.add_argument("--users", type=Path, help=f"the user list file (default: {USERS_FILE} in the Win64 folder)")
    usub.add_parser("list", parents=[where], help="show the users (never their keys)")
    ua = usub.add_parser("add", parents=[where], help="add a user and print their new key once")
    ua.add_argument("name")
    ua.add_argument("--create", action="store_true", help="let them start games")
    ua.add_argument("--key", help="use this key (from 'key') instead of making one")
    ur = usub.add_parser("rename", parents=[where], help="change a user's name; they must then join with it")
    ur.add_argument("old")
    ur.add_argument("new")
    uc = usub.add_parser("create", parents=[where], help="let a user start games (on) or not (off)")
    uc.add_argument("name")
    uc.add_argument("state", choices=("on", "off"))
    uv = usub.add_parser("revoke", parents=[where], help="remove a user")
    uv.add_argument("name")
    uk = usub.add_parser("require-keys", parents=[where], help="on: joining needs name and key; off: anyone may join")
    uk.add_argument("state", choices=("on", "off"))
    a = p.parse_args(argv)
    try:
        if a.cmd == "release":
            print(f"RELEASE OK {release(build=not a.no_build)}  (VM: python -m tools.vm update)")
            return 0
        if a.cmd == "key":
            make_key(a.label)
            return 0
        if a.cmd == "user":
            return _run_user(a)
        return update(a.version, a.win64, start=not a.no_start, checked_out=a.checked_out)
    except VmError as e:
        print(f"PROBLEM: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())

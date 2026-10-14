"""Update the rented server machine (the VM) without copying the whole server kit.

On the laptop, after pushing the commit:

    python -m tools.vm release              # build the mod, upload it as release <commit>
    python -m tools.vm release --no-build   # upload the DLLs already built

On the VM, from its clone of this repo (Claude there runs it):

    python -m tools.vm update               # newest release
    python -m tools.vm update --version abc1234 --no-start

Only the mod (reborn.dll, reborn.pdb, dxgi.dll, about 11 MB) travels, through the private
repo Sintax/reborn-builds. The VM checks out the same commit from GitHub, so its scripts and
matchmaking service match the mod; it builds the service itself with its own .NET. The Steam
loader files already sit on the VM and never travel. Instructions: docs/TWO-MACHINES.md.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

from . import config

BUILDS_REPO = "Sintax/reborn-builds"
MOD_FILES = ("reborn.dll", "reborn.pdb", "dxgi.dll")
DEPLOYED_MARK = "reborn-deployed.txt"
GAMES_URL = "http://localhost:5000/api/games"


class VmError(RuntimeError):
    pass


def run(cmd: list[str], cwd: Path = config.REPO, check: bool = True) -> str:
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if check and p.returncode != 0:
        raise VmError(f"{' '.join(cmd)} failed:\n{p.stdout}{p.stderr}")
    return p.stdout.strip()


def git(*args: str, check: bool = True) -> str:
    return run(["git", *args], check=check)


# --- laptop ---------------------------------------------------------------------------------

def mod_files() -> list[Path]:
    from . import package
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


def release(build: bool) -> str:
    version = check_pushed()
    if build:
        from . import package
        package.build_all()
    files = [str(f) for f in mod_files()]
    notes = f"{git('rev-parse', '--abbrev-ref', 'HEAD')}: {git('log', '-1', '--format=%s')}"
    exists = subprocess.run(["gh", "release", "view", version, "--repo", BUILDS_REPO],
                            capture_output=True, text=True).returncode == 0
    if exists:
        run(["gh", "release", "upload", version, *files, "--repo", BUILDS_REPO, "--clobber"])
    else:
        run(["gh", "release", "create", version, *files, "--repo", BUILDS_REPO,
             "--title", version, "--notes", notes, "--latest"])
    return version


# --- VM -------------------------------------------------------------------------------------

def latest_version() -> str:
    out = run(["gh", "release", "view", "--repo", BUILDS_REPO, "--json", "tagName"])
    return json.loads(out)["tagName"]


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
    run(["gh", "release", "download", version, "--repo", BUILDS_REPO, "--dir", str(scratch)])
    for name in MOD_FILES:
        if not (scratch / name).exists():
            raise VmError(f"release {version} has no {name}")
    return scratch


def install_mod(downloaded: Path, win64: Path) -> None:
    for name in MOD_FILES:
        shutil.copy2(downloaded / name, win64 / name)


def install_scripts(version: str, win64: Path) -> None:
    from . import package, serverkit
    for name in serverkit.SCRIPTS:
        (win64 / name).write_bytes(serverkit.script(name, version).encode("utf-8"))
    (win64 / "CollectLogs.bat").write_bytes(serverkit.crlf(package.COLLECT_LOGS_BAT).encode("utf-8"))
    (win64 / "README-SERVER.txt").write_bytes(serverkit.readme(version).encode("utf-8"))


def build_matchmaking(scratch: Path) -> Path:
    """Build the service with this machine's .NET (small; the runtime is not inside)."""
    from . import serverkit
    return serverkit.publish_coordinator(scratch, self_contained=False)


def install_matchmaking(published: Path, win64: Path) -> None:
    """The owner's appsettings.json (which games run) is kept; a first install gets the repo's."""
    dest = win64 / "matchmaking"
    kept = (dest / "appsettings.json").read_bytes() if (dest / "appsettings.json").exists() else None
    if dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(published, dest)
    if kept is not None:
        (dest / "appsettings.json").write_bytes(kept)


def start_matchmaking(win64: Path) -> None:
    subprocess.Popen(["cmd", "/c", str(win64 / "StartMatchmaking.bat")], cwd=win64,
                     creationflags=subprocess.CREATE_NEW_CONSOLE | subprocess.CREATE_NEW_PROCESS_GROUP)


def wait_for_game(seconds: int = 180) -> list[dict]:
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            with urllib.request.urlopen(GAMES_URL, timeout=5) as r:
                games = json.load(r)
            if games:
                return games
        except OSError:
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
    version = version or latest_version()
    if not checked_out:
        checkout(version)
        # Carry on with the code of the version just checked out, not the code that started.
        return subprocess.call([sys.executable, "-m", "tools.vm", "update", "--version", version,
                                "--win64", str(win64), "--checked-out", *([] if start else ["--no-start"])],
                               cwd=config.REPO)
    print(f"Updating the server to {version}")
    scratch = Path(os.environ.get("TEMP", str(config.REPO))) / "reborn-vm-update"
    downloaded = download_mod(version, scratch / "mod")      # both before stopping anything,
    published = build_matchmaking(scratch / "matchmaking")  # so a failure leaves the server up
    stop_servers()
    install_mod(downloaded, win64)
    install_scripts(version, win64)
    install_matchmaking(published, win64)
    (win64 / DEPLOYED_MARK).write_text(version + "\n", encoding="utf-8")
    print(f"Installed {version} into {win64}")
    for rule in firewall_rules_missing():
        print(f"WARNING: Windows Firewall rule '{rule}' is missing; run OpenFirewall.bat once")
    if not start:
        print("UPDATE OK (not started)")
        return 0
    start_matchmaking(win64)
    games = wait_for_game()
    if not games:
        log = win64 / "matchmaking" / "matchmaking.log"
        print(f"PROBLEM: no game listed at {GAMES_URL} after 3 minutes; read {log} "
              f"and the newest rb-* folder in {config.LOG_DIR}")
        if log.exists():
            print("Last lines of the matchmaking log:")
            print("\n".join(log.read_text(encoding="utf-8", errors="replace").splitlines()[-20:]))
        return 1
    for g in games:
        print(f"Game listed: {g.get('instanceName')}  {g.get('serverConnectString')}")
    deadline = time.time() + 90   # the game is listed as soon as Serverborn starts; it listens ~20 s later
    while not newest_log_says_listening() and time.time() < deadline:
        time.sleep(5)
    if not newest_log_says_listening():
        print("NOTE: after 90 s the newest server log still does not say it is listening; check it")
    print("UPDATE OK")
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
    a = p.parse_args(argv)
    try:
        if a.cmd == "release":
            print(f"RELEASE OK {release(build=not a.no_build)}  (VM: python -m tools.vm update)")
            return 0
        return update(a.version, a.win64, start=not a.no_start, checked_out=a.checked_out)
    except VmError as e:
        print(f"PROBLEM: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())

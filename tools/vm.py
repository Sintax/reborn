"""Update the rented server machine (the VM) without copying the whole server kit.

On the laptop, after pushing the commit:

    python -m tools.vm release              # build the mod, upload it as release <commit>
    python -m tools.vm release --no-build   # upload the DLLs already built

On the VM, from its clone of this repo (Claude there runs it):

    python -m tools.vm update               # newest release
    python -m tools.vm update --version abc1234 --no-start

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

Only the mod (reborn.dll, reborn.pdb, dxgi.dll, about 11 MB) travels, through the private
repo Sintax/reborn-builds. The VM checks out the same commit from GitHub, so its scripts and
matchmaking service match the mod; it builds the service itself with its own .NET. The Steam
loader files already sit on the VM and never travel. Instructions: docs/TWO-MACHINES.md.
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
import time
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

from . import config

BUILDS_REPO = "Sintax/reborn-builds"
MOD_FILES = ("reborn.dll", "reborn.pdb", "dxgi.dll")
DEPLOYED_MARK = "reborn-deployed.txt"
GAMES_URL = "http://localhost:5000/api/games"
KEY_ALPHABET = "ABCDEFGHJKMNPQRSTVWXYZ23456789"   # no 0/O/1/I/L: friends may type it
KEY_LENGTH = 24
KEYS_DIR = config.REPO / "dist" / "keys"          # dist/ is gitignored
USERS_FILE = "reborn-users.json"
NAME_RE = re.compile(r"[A-Za-z0-9_-]{1,24}")


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
    if (not isinstance(data, dict) or not isinstance(data.get("Users"), list)
            or not all(isinstance(u, dict) and isinstance(u.get("Name"), str) for u in data["Users"])):
        raise VmError(f"{path} is not a user list (expected RequireKeys and Users); fix or remove it first")
    data.setdefault("RequireKeys", True)
    return data


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

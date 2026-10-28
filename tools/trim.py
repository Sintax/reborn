"""Find out which game files a server can do without.

A server does not need every file the game ships (movies, sound, the debug builds of the physics
library...). This tool makes a copy of the game folder with chosen parts left out, runs the
regression games with the server started from that copy, and records which parts were safe to leave
out in tools/trim_results.json. The installer offers only the parts recorded as passed.

The copy costs almost no disk space: everything is a hard link (a second name for the same file on
disk) except the few folders a running game writes into, which are real copies. Deleting the copy
removes only the extra names; the real game folder is never changed.

  python -m tools.trim test --part movies     try one part
  python -m tools.trim test --all-passed      try every part that passed, all removed together
  python -m tools.trim test --clean           delete a copy left behind by a crash
  python -m tools.trim measure                show how big each part is
"""
import argparse
import json
import os
import re
import configparser
import shutil
import subprocess
import sys
import time
from datetime import date
from pathlib import Path
from typing import Callable

import psutil

from . import config

PARTS_FILE = Path(__file__).resolve().parent / "trim_parts.json"
RESULTS_FILE = Path(__file__).resolve().parent / "trim_results.json"
COPY_NAME = "Battleborn-trimtest"
MARKER = ".trimtest-copy"
SCENARIOS = ("s1-dojo-1client-smoke", "s2-algorithm-2clients-smoke", "s3-meltdown-2clients-bots-smoke")
ALL_PASSED = "all-passed"

# Folders (relative to the game folder) a running game may write into: these are real copies, so
# nothing the test server writes can reach the real game files through a shared hard link.
REAL_COPY_DIRS = ("Binaries/Win64", "Engine/Config", "Gearbox/Config", "PoplarGame/Config", "WillowGame/Config")
# Small settings-like files a game may rewrite (shader caches, manifests, ini files): real copies too.
# Whatever this list gets wrong, run_trial's before-and-after check of the real files catches.
REAL_COPY_SUFFIXES = (".bin", ".ini", ".txt", ".xml", ".json", ".int", ".usf", ".bak", ".cfg", ".log")
# Inside Win64: the runner's own working folders (several GB of loader copies and old DLLs). The
# runner makes what it needs again, so the copy leaves them out; they never count as game files.
SKIPPED_DIRS = ("Binaries/Win64/rb_ids", "Binaries/Win64/rb_backups")


class TrimError(Exception):
    """A problem with a plain-language message."""


class HarnessProblem(TrimError):
    """The regression run itself could not run (not a verdict on the part)."""


# ---------------------------------------------------------------- parts and matching

def load_parts(path: Path = PARTS_FILE) -> list[dict]:
    return json.loads(path.read_text(encoding="utf-8"))


def find_part(part_id: str, parts: list[dict]) -> dict:
    for p in parts:
        if p["id"] == part_id:
            return p
    raise TrimError(f"unknown part '{part_id}'; known parts: {', '.join(p['id'] for p in parts)}")


def _glob_regex(glob: str) -> re.Pattern:
    """Glob -> regex over a '/'-separated path relative to the game folder, any letter case.
    `**` crosses folders, `**/` also matches no folder at all, `*` and `?` stay inside one name."""
    g = glob.replace("\\", "/").lstrip("/")
    out, i = [], 0
    while i < len(g):
        if g.startswith("**/", i):
            out.append("(?:.*/)?")
            i += 3
        elif g.startswith("**", i):
            out.append(".*")
            i += 2
        elif g[i] == "*":
            out.append("[^/]*")
            i += 1
        elif g[i] == "?":
            out.append("[^/]")
            i += 1
        else:
            out.append(re.escape(g[i]))
            i += 1
    return re.compile("".join(out) + r"\Z", re.IGNORECASE)


def _matcher(parts: list[dict]) -> Callable[[str], bool]:
    rx = [_glob_regex(g) for p in parts for g in p["globs"]]
    return lambda rel: any(r.match(rel) for r in rx)


def _is_link(p: str) -> bool:
    return os.path.islink(p) or os.path.isjunction(p)


def _under(rel: str, folder: str) -> bool:
    low, f = rel.lower(), folder.lower()
    return low == f or low.startswith(f + "/")


def walk_game(game: Path):
    """Yield (relative path with '/', is_dir) for everything in the game folder except the
    runner's working folders. A link folder is refused: a copy that followed it would be wrong."""
    root = str(game)
    for cur, dirs, files in os.walk(root):
        rel_cur = os.path.relpath(cur, root).replace("\\", "/")
        rel_cur = "" if rel_cur == "." else rel_cur
        keep = []
        for d in dirs:
            rel = f"{rel_cur}/{d}" if rel_cur else d
            if _is_link(os.path.join(cur, d)):
                raise TrimError(f"{rel} is a link to another folder; the trim test cannot copy that")
            if any(_under(rel, s) for s in SKIPPED_DIRS):
                continue
            keep.append(d)
            yield rel, True
        dirs[:] = keep
        for f in files:
            rel = f"{rel_cur}/{f}" if rel_cur else f
            if _is_link(os.path.join(cur, f)):
                raise TrimError(f"{rel} is a link; the trim test cannot copy that")
            yield rel, False


def matching_files(game: Path, parts: list[dict]) -> list[str]:
    hit = _matcher(parts)
    return [rel for rel, is_dir in walk_game(game) if not is_dir and hit(rel)]


def measure(game: Path, part: dict) -> int:
    """Bytes of the files in the game folder that the part matches."""
    return sum(os.path.getsize(game / rel) for rel in matching_files(game, [part]))


# ---------------------------------------------------------------- the copy

def _same_volume(a: Path, b: Path) -> bool:
    return os.stat(a).st_dev == os.stat(b).st_dev


def _nearest_existing(p: Path) -> Path:
    while not p.exists():
        if p.parent == p:
            break
        p = p.parent
    return p


def _is_real_copy(rel: str) -> bool:
    return any(_under(rel, f) for f in REAL_COPY_DIRS) or rel.lower().endswith(REAL_COPY_SUFFIXES)


def snapshot_linked(src: Path, remove: list[dict]) -> dict[str, tuple[int, int]]:
    """(size, modified time) of every real game file the copy will share by hard link. A running
    game that writes into such a file changes the real file too; this is how that is noticed."""
    hit = _matcher(remove)
    snap = {}
    for rel, is_dir in walk_game(src):
        if is_dir or hit(rel) or _is_real_copy(rel):
            continue
        st = os.stat(Path(src) / rel)
        snap[rel] = (st.st_size, st.st_mtime_ns)
    return snap


def changed_files(src: Path, snap: dict[str, tuple[int, int]]) -> list[str]:
    out = []
    for rel, was in snap.items():
        try:
            st = os.stat(Path(src) / rel)
        except OSError:
            out.append(rel)
            continue
        if (st.st_size, st.st_mtime_ns) != was:
            out.append(rel)
    return out


def make_trimmed_copy(src: Path, dst: Path, remove: list[dict]) -> None:
    """Copy `src` to `dst`. Binaries\\Win64, the small Config folders and settings-like files (.bin, .ini ...) are
    real copies, every other file is a hard link, and files matched by `remove` are left out."""
    src, dst = Path(src), Path(dst)
    if not (src / "Binaries" / "Win64" / "Battleborn.exe").is_file():
        raise TrimError(f"{src} is not a Battleborn folder (Binaries\\Win64\\Battleborn.exe is missing)")
    if dst.exists():
        raise TrimError(f"{dst} already exists. Delete it first (python -m tools.trim test --clean).")
    anchor = _nearest_existing(dst.parent)
    if not _same_volume(src, anchor):
        raise TrimError(f"{dst} is on a different drive than the game folder; the test copy has to be on "
                        "the same drive so it can share files instead of using another 40 GB.")
    hit = _matcher(remove)
    dst.mkdir(parents=True)   # outside the try: if someone else made it meanwhile, it is not ours to delete
    try:
        for rel, is_dir in walk_game(src):
            if is_dir:
                (dst / rel).mkdir(exist_ok=True)
                continue
            if hit(rel):
                continue
            s, d = src / rel, dst / rel
            if _is_real_copy(rel):
                shutil.copy2(s, d)
            else:
                os.link(s, d)
        (dst / MARKER).write_text("A test copy made by tools.trim. Safe to delete.\n", encoding="utf-8")
    except BaseException:
        remove_copy(dst, force=True)
        raise


DELETE_TRIES = 5
DELETE_WAIT_S = 1.0


def _delete_contents(dst: Path) -> list[str]:
    """Delete everything in dst except the marker; return what could not be deleted."""
    errors: list[str] = []
    for child in dst.iterdir():
        if child.name == MARKER:
            continue
        try:
            if child.is_dir() and not _is_link(str(child)):
                shutil.rmtree(child, onexc=lambda fn, path, exc: errors.append(f"{path}: {exc}"))
            else:
                child.unlink()
        except OSError as e:
            errors.append(f"{child}: {e}")
    return errors


def remove_copy(dst: Path, force: bool = False) -> None:
    """Delete a test copy. Only a folder carrying our marker is ever deleted (`force` is for a copy
    whose marker was not written yet). Deleting removes the extra names of hard-linked files only;
    the files in the real game folder stay. Attributes are never changed: they are shared.
    A file still in use (a game slow to exit) is retried for a few seconds. The marker goes last,
    so a copy that could not be deleted can still be cleaned with --clean later."""
    dst = Path(dst)
    if not dst.exists():
        return
    if not force and not (dst / MARKER).is_file():
        raise TrimError(f"{dst} is not a trim test copy (no marker file); not deleting it")
    for attempt in range(DELETE_TRIES):
        errors = _delete_contents(dst)
        if not errors:
            break
        if attempt < DELETE_TRIES - 1:
            time.sleep(DELETE_WAIT_S)
    if errors:
        raise TrimError(f"could not fully delete {dst}: {errors[0]}. Close any game still running, "
                        "then run: python -m tools.trim test --clean")
    (dst / MARKER).unlink(missing_ok=True)
    dst.rmdir()


# ---------------------------------------------------------------- results

def load_results(path: Path = RESULTS_FILE) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return {}


def write_results(results: dict, path: Path = RESULTS_FILE) -> None:
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    os.replace(tmp, path)


def _commit() -> str:
    try:
        r = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=config.REPO,
                           capture_output=True, text=True, timeout=30)
        return r.stdout.strip() or "unknown"
    except (OSError, subprocess.SubprocessError):
        return "unknown"


def record(results_path: Path, key: str, scenarios: dict, nbytes: int, commit: str | None = None,
           globs: list[str] | None = None) -> dict:
    """`globs` are the file patterns that were left out in this test. The installer offers a part
    only while its patterns in trim_parts.json are still exactly these: a changed part needs a new test."""
    entry = {"passed": bool(scenarios) and all(k == "pass" for k in scenarios.values()),
             "bytes": nbytes, "scenarios": dict(scenarios), "globs": list(globs or []),
             "commit": commit or _commit(), "date": date.today().isoformat()}
    results = load_results(results_path)
    results[key] = entry
    write_results(results, results_path)
    return entry


# ---------------------------------------------------------------- running the games

Runner = Callable[[str, Path], str]   # (scenario name, server game folder) -> outcome kind ("pass" ...)


SERVER_POLL_S = 0.5


def server_exes() -> list[str]:
    """Full path of every running Serverborn.exe."""
    out = []
    for p in psutil.process_iter(["name", "exe"]):
        if p.info["name"] == "Serverborn.exe" and p.info["exe"]:
            out.append(p.info["exe"])
    return out


def _inside(path: str | Path, folder: Path) -> bool:
    a, b = os.path.normcase(os.path.realpath(path)), os.path.normcase(os.path.realpath(folder))
    return a == b or a.startswith(b + os.sep)


def _loader_records_copy(copy: Path) -> bool:
    """The launcher's own record: a server identity in the copy that points at the copy's Serverborn.exe."""
    for ini in (copy / "Binaries" / "Win64" / "rb_ids").glob("*/ColdClientLoader.ini"):
        cp = configparser.ConfigParser(interpolation=None)
        cp.optionxform = str
        try:
            cp.read(ini, encoding="utf-8")
            exe = cp["SteamClient"]["Exe"]
        except (configparser.Error, KeyError, OSError):
            continue
        if exe.lower().endswith("serverborn.exe") and _inside(exe, copy):
            return True
    return False


def check_server_location(copy: Path, seen: list[str], scenario: str) -> None:
    """A pass only counts when the server really ran from the trimmed copy. Seen running servers
    must all be inside the copy; a run too short to be seen must at least have been launched from it."""
    outside = [p for p in seen if not _inside(p, copy)]
    if outside:
        raise HarnessProblem(f"the test server for {scenario} ran from {outside[0]}, not from the "
                             f"trimmed copy {copy}; the result would say nothing about the removed files")
    if not seen and not _loader_records_copy(copy):
        raise HarnessProblem(f"could not confirm that the test server for {scenario} ran from the "
                             f"trimmed copy {copy}; no result recorded")


def real_runner(scenario: str, server_game: Path, popen=subprocess.Popen, list_servers=server_exes,
                interval: float = SERVER_POLL_S) -> str:
    """Run one regression game with the server started from `server_game`; players use the normal folder."""
    env = {**os.environ, "BB_SERVER_GAME_DIR": str(server_game)}
    proc = popen([sys.executable, "-m", "debugloop.run", scenario], cwd=config.REPO, env=env,
                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    seen: list[str] = []
    while True:
        try:
            out, err = proc.communicate(timeout=interval)
            break
        except subprocess.TimeoutExpired:
            seen.extend(p for p in list_servers() if p not in seen)
    out = out or ""
    sys.stdout.write(out[-2000:])
    last = (out.strip().splitlines() or (err or "").strip().splitlines() or ["no message"])[-1]
    if proc.returncode not in (0, 1):
        raise HarnessProblem(f"the test run could not start ({scenario}): {last}")
    check_server_location(server_game, seen, scenario)
    if proc.returncode == 0:
        return "pass"
    start = out.find("{")
    try:
        return str(json.loads(out[start:])["outcome"]["kind"])
    except (ValueError, KeyError, TypeError):
        raise HarnessProblem(f"the test run for {scenario} stopped without a result (a fault in the "
                             f"test tool, not a verdict on the part): {last}") from None


def run_trial(src: Path, dst: Path, remove: list[dict], runner: Runner,
              scenarios: tuple[str, ...] = SCENARIOS) -> dict:
    """Make the copy, run the scenarios until one fails, always delete the copy. If a real game file
    changed while the games ran (written through a shared link), nothing counts: HarnessProblem."""
    snap = snapshot_linked(src, remove)
    make_trimmed_copy(src, dst, remove)
    try:
        outcomes: dict[str, str] = {}
        for name in scenarios:
            outcomes[name] = runner(name, dst)
            if outcomes[name] != "pass":
                break
        changed = changed_files(src, snap)
        if changed:
            more = f" (and {len(changed) - 1} more)" if len(changed) > 1 else ""
            raise HarnessProblem(
                f"the real game file {changed[0]}{more} changed while the test ran: the test server wrote "
                "into a file it shares with the real game folder. No result was recorded. Use Verify "
                "integrity of game files in Steam to repair the game, and make that file a real copy.")
        return outcomes
    finally:
        try:
            remove_copy(dst)
        except TrimError as e:
            print(f"WARNING: {e}")   # the verdict stands; the copy is cleaned later with --clean


def test_parts(part_id: str | None, all_passed: bool, game: Path, runner: Runner,
               parts_path: Path = PARTS_FILE, results_path: Path = RESULTS_FILE,
               scenarios: tuple[str, ...] = SCENARIOS) -> dict:
    """Try one part, or every part recorded as passed, all removed together. Records and returns the result."""
    parts = load_parts(parts_path)
    if all_passed:
        done = load_results(results_path)
        chosen = [p for p in parts if done.get(p["id"], {}).get("passed")]
        if not chosen:
            raise TrimError("no part has passed yet; test the parts one by one first (--part <id>)")
        key = ALL_PASSED
    else:
        chosen = [find_part(part_id, parts)]
        key = chosen[0]["id"]
    nbytes = sum(os.path.getsize(game / rel) for rel in matching_files(game, chosen))
    dst = game.parent / COPY_NAME
    outcomes = run_trial(game, dst, chosen, runner, scenarios)
    globs = [g for p in chosen for g in p["globs"]]
    return {key: record(results_path, key, outcomes, nbytes, globs=globs)}


# ---------------------------------------------------------------- command line

def _gb(n: int) -> str:
    return f"{n / 1024 ** 3:.2f} GB"


def main(argv: list[str] | None = None, runner: Runner = real_runner) -> int:
    ap = argparse.ArgumentParser(prog="python -m tools.trim", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("test", help="run the regression games on a trimmed copy")
    g = t.add_mutually_exclusive_group(required=True)
    g.add_argument("--part", help="id of one part (see tools/trim_parts.json)")
    g.add_argument("--all-passed", action="store_true", help="remove every part that passed, together")
    g.add_argument("--clean", action="store_true", help="delete a leftover test copy")
    sub.add_parser("measure", help="show the size of each part")
    a = ap.parse_args(argv)
    game = config.GAME_DIR
    try:
        if a.cmd == "measure":
            for p in load_parts():
                print(f"{p['id']:<15} {p['label']:<20} {_gb(measure(game, p))}")
            return 0
        if a.clean:
            remove_copy(game.parent / COPY_NAME)
            print("test copy deleted (or there was none)")
            return 0
        res = test_parts(a.part, a.all_passed, game, runner)
    except HarnessProblem as e:
        print(f"HARNESS ERROR: {e}")
        return 2
    except TrimError as e:
        print(f"ERROR: {e}")
        return 2
    except OSError as e:
        print(f"ERROR: could not make or delete the test copy: {e}. The test copy needs a drive that "
              "keeps hard links (NTFS) with enough free space.")
        return 2
    for key, e in res.items():
        print(f"{key}: {'PASSED' if e['passed'] else 'FAILED'} ({_gb(e['bytes'])}) {e['scenarios']}")
    return 0 if all(e["passed"] for e in res.values()) else 1


if __name__ == "__main__":
    sys.exit(main())

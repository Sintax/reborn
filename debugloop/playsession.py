"""A scenario left running for live driving (the battleborn-play MCP / debugloop.play), outside the
graded loop.

`loop play <scenario>` builds, deploys and starts it, waits for the players to be in the game,
records pids and ports in state/play.json and exits with the games still running.
`loop stop-play` ends them. `loop next` / `verify` refuse while a session is alive.
"""
from __future__ import annotations

import json
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

from . import config, launch, run, screenshot
from .outcome import STARTUP_TIMEOUT_S
from .state import atomic_write

PLAY_FILE = "play.json"


def _file(state_dir: Path) -> Path:
    return state_dir / PLAY_FILE


def alive(state_dir: Path = config.STATE_DIR) -> dict | None:
    """play.json's content while any process it names is still the one we started, else None."""
    f = _file(state_dir)
    if not f.exists():
        return None
    try:
        info = json.loads(f.read_text(encoding="utf-8"))
        recs = info.get("pids", {}).values()
    except (ValueError, OSError, AttributeError):
        return None
    return info if any(run._is_same_process(r) for r in recs) else None


def _kill(handles) -> None:
    for h in handles:
        try:
            h.kill()
        finally:
            h.close()


def start(scn, launcher=None, runs_dir: Path = config.RUNS_DIR, state_dir: Path = config.STATE_DIR,
          poll_s: float = 2.0, wait_s: float = STARTUP_TIMEOUT_S, arrange=None, keeper=None) -> dict:
    if alive(state_dir):
        raise run.HarnessError("a play session is already running; end it with "
                               "`python -m debugloop.loop stop-play`")
    # Window placement only for games really launched here (fake launchers use made-up pids).
    if launcher is None:
        arrange = arrange or screenshot.arrange
        keeper = keeper or _start_keeper
    launcher = launcher or launch.RealLauncher()
    run.preconditions(runs_dir, len(scn.processes))
    run_dir = runs_dir / f"{datetime.now():%Y%m%d-%H%M%S}-play-{scn.name}"
    try:
        run_dir.mkdir(parents=True)
    except OSError as e:
        raise run.HarnessError(f"cannot create {run_dir}: {e}") from e
    first = config.FIRST_DEBUG_PORT
    ports = {p.name: first + i for i, p in enumerate(scn.processes)}
    n_clients = sum(p.role == "client" for p in scn.processes)
    handles: dict[str, launch.ProcessHandle] = {}
    info = {"run_dir": str(run_dir), "scenario": scn.name, "ports": ports, "pids": {}}

    def remember():
        info["pids"] = {n: run._identity(h.pid) or {"pid": h.pid} for n, h in handles.items()}
        atomic_write(_file(state_dir), json.dumps(info, indent=2))

    def tidy():
        if arrange:
            try:
                arrange({n: h.pid for n, h in handles.items()})
            except Exception:
                pass   # window placement is cosmetic

    def launch_one(spec):
        handles[spec.name] = launcher.start(spec.name, spec.role,
                                            run._args(spec, ports[spec.name], run_dir, n_clients))
        remember()

    try:
        t0 = time.time()
        for spec in [p for p in scn.processes if p.role == "server"]:
            launch_one(spec)
            while time.time() - t0 < wait_s:
                st, _ = run._get_state(ports[spec.name])
                if st and st.get("listening"):
                    break
                tidy()
                time.sleep(poll_s)
        clients = [p for p in scn.processes if p.role != "server"]
        for spec in clients:
            launch_one(spec)
        while clients and time.time() - t0 < wait_s:
            tidy()
            states = [run._get_state(ports[p.name])[0] or {} for p in clients]
            if all(s.get("autopilot") == "playing" or s.get("has_pawn") for s in states):
                break
            time.sleep(poll_s)
        tidy()
    except (RuntimeError, OSError) as e:
        _kill(handles.values())
        _file(state_dir).unlink(missing_ok=True)
        raise run.HarnessError(str(e)) from e
    for h in handles.values():
        h.close()   # play.json holds the pids; stop() reopens them
    if keeper:
        try:
            keeper(state_dir)
        except Exception:
            pass   # window placement is cosmetic
    return info


def _start_keeper(state_dir: Path) -> None:
    """Keep the windows arranged after this command exits (debugloop.windowkeeper)."""
    flags = getattr(subprocess, "DETACHED_PROCESS", 0) | getattr(subprocess, "CREATE_NO_WINDOW", 0)
    subprocess.Popen([sys.executable, "-m", "debugloop.windowkeeper"], cwd=config.REPO,
                     creationflags=flags, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL)


def stop(state_dir: Path = config.STATE_DIR) -> list[str]:
    """Kill the session's processes (only ones that are still ours) and forget the session."""
    info = alive(state_dir)
    killed = []
    for name, rec in (info or {}).get("pids", {}).items():
        if not run._is_same_process(rec):
            continue
        try:
            h = launch.ProcessHandle(int(rec["pid"]))
        except OSError:
            continue
        _kill([h])
        killed.append(name)
    _file(state_dir).unlink(missing_ok=True)
    return killed

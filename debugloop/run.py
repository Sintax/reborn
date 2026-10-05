import argparse
import http.client
import json
import os
import shutil
import socket
import sys
import time
import urllib.error
import urllib.request
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path

import psutil

from . import config, launch, scenario, screenshot, signature
from .outcome import STARTUP_TIMEOUT_S, Outcome, ProcessRecord, Sample, classify, milestone


class HarnessError(Exception):
    pass


@dataclass
class RunResult:
    run_id: str
    scenario: str
    outcome: Outcome
    signature: str | None
    run_dir: Path
    elapsed_s: float
    milestone: int = 0   # how far the join got (outcome.milestone); the loop's "got further" test


def _get_state(port: int) -> tuple[dict | None, int | None]:
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/state", timeout=4) as r:
            return json.loads(r.read()), r.status
    except urllib.error.HTTPError as e:
        return None, e.code
    except (urllib.error.URLError, TimeoutError, ConnectionError, http.client.HTTPException, ValueError):
        return None, None


def _identity(pid: int) -> dict | None:
    """pid + start time + exe name: enough to tell our process from a reused pid."""
    try:
        p = psutil.Process(pid)
        return {"pid": pid, "create_time": p.create_time(), "exe": p.name()}
    except psutil.Error:
        return None


def _is_same_process(rec) -> bool:
    if not isinstance(rec, dict) or "create_time" not in rec or "exe" not in rec or "pid" not in rec:
        return False  # old format or damaged: never kill on a guess
    try:
        p = psutil.Process(int(rec["pid"]))
        return abs(p.create_time() - float(rec["create_time"])) <= 1.0 and p.name() == rec["exe"]
    except (psutil.Error, ValueError, TypeError):
        return False


def _reap_stale(runs_dir: Path) -> None:
    f = runs_dir / "active.json"
    if not f.exists():
        return
    try:
        recs = json.loads(f.read_text(encoding="utf-8")).get("pids", {}).values()
    except (ValueError, AttributeError, OSError):
        print(f"note: {f} was unreadable; deleting it without stopping anything")
        recs = []
    for rec in recs:
        if _is_same_process(rec):
            try:
                h = launch.ProcessHandle(int(rec["pid"]))
            except OSError:
                continue
            try:
                h.kill()
            finally:
                h.close()
    f.unlink(missing_ok=True)


def _port_in_use(port: int) -> bool:
    s = socket.socket()
    try:
        s.bind(("127.0.0.1", port))
        return False
    except OSError:
        return True
    finally:
        s.close()


def preconditions(runs_dir: Path, n_processes: int = 0) -> None:
    """Checks before anything is built, deployed or started: stale runs reaped, no game already
    running, enough disk space, and the debug ports for n_processes free on 127.0.0.1."""
    try:
        runs_dir.mkdir(parents=True, exist_ok=True)
        _reap_stale(runs_dir)
    except OSError as e:
        raise HarnessError(f"cannot prepare {runs_dir}: {e}") from e
    foreign = launch.find_game_processes()
    if foreign:
        raise HarnessError(f"a game process is already running (pids {foreign}); "
                           "close it first - the runner only stops games it started")
    try:
        free_mb = shutil.disk_usage(runs_dir).free // (1024 * 1024)
    except OSError as e:
        raise HarnessError(f"cannot check disk space: {e}") from e
    if free_mb < config.MIN_FREE_MB:
        raise HarnessError(f"only {free_mb} MB free disk space")
    busy = [p for p in range(config.FIRST_DEBUG_PORT, config.FIRST_DEBUG_PORT + n_processes)
            if _port_in_use(p)]
    if busy:
        raise HarnessError(f"debug port(s) {busy} already in use on 127.0.0.1; "
                           "another program holds them")


def _args(spec, port: int, run_dir: Path, n_clients: int) -> list[str]:
    a = launch.base_args(spec.role) + list(spec.args) + [
        f"-rbinstance={spec.name}", f"-rbdebugport={port}", f"-rbrundir={run_dir}"]
    if spec.role == "server":
        a.append(f"-rbplayers={n_clients}")
    if spec.role == "client":
        a.append(f"-rbjoin=127.0.0.1:{config.SERVER_PORT}")
    return a


def _records(scn, handles, run_dir: Path, killed: frozenset | set = frozenset()) -> list[ProcessRecord]:
    """Exit codes count only for processes that exited on their own; ones the runner killed read None."""
    recs = []
    for spec in scn.processes:
        crashes = [_report(p, {"first_chance": True, "frames": []})
                   for p in sorted(run_dir.glob(f"{spec.name}.*.crash.json"))]
        hang_f = run_dir / f"{spec.name}.hang.json"
        log_f = run_dir / f"{spec.name}.log"
        tail = log_f.read_text(errors="replace").splitlines()[-80:] if log_f.exists() else []
        h = handles.get(spec.name)
        recs.append(ProcessRecord(spec.name, spec.role, h.exit_code() if h and spec.name not in killed else None, crashes,
                                  _report(hang_f, {"frames": []}) if hang_f.exists() else None, tail))
    return recs


def _report(p: Path, fallback: dict) -> dict:
    """A crash or hang report. One that cannot be read (half written, locked, bad bytes) is an
    "unreadable report", never a crash of the runner. An unreadable crash report counts as
    first-chance, so it is graded a crash only if the process then dies."""
    try:
        r = json.loads(p.read_text(encoding="utf-8", errors="replace"))
        if isinstance(r, dict):
            return r
    except (ValueError, OSError):
        pass
    return {"unreadable": p.name, **fallback}


def run_scenario(scn, launcher=None, runs_dir: Path = config.RUNS_DIR, poll_s: float = 2.0,
                 shots=None) -> RunResult:
    # Screenshots only of games this runner really launched (fake launchers use made-up pids).
    if shots is None and launcher is None:
        shots = screenshot.capture_all
    launcher = launcher or launch.RealLauncher()
    preconditions(runs_dir, len(scn.processes))
    base_id = f"{datetime.now():%Y%m%d-%H%M%S}-{scn.name}"
    try:
        run_id, n = base_id, 1
        while True:
            run_dir = runs_dir / run_id
            try:
                run_dir.mkdir(parents=True)
                break
            except FileExistsError:
                n += 1
                run_id = f"{base_id}-{n}"
        if scn.path.exists():
            shutil.copy2(scn.path, run_dir / "scenario.toml")
    except OSError as e:
        raise HarnessError(f"cannot create run folder: {e}") from e
    ports = {p.name: config.FIRST_DEBUG_PORT + i for i, p in enumerate(scn.processes)}
    n_clients = sum(p.role == "client" for p in scn.processes)
    handles: dict[str, launch.ProcessHandle] = {}
    killed: set[str] = set()
    samples: list[Sample] = []
    active = runs_dir / "active.json"
    t0 = time.time()
    next_shot = [screenshot.SHOT_EVERY_S]

    def take_shots(label: str):
        if shots:
            try:
                shots({n: h.pid for n, h in handles.items()}, run_dir, label)
            except Exception:
                pass   # a missing picture must never fail a run

    def save_active():
        pids = {n: _identity(h.pid) or {"pid": h.pid} for n, h in handles.items()}
        tmp = active.with_name("active.json.tmp")
        tmp.write_text(json.dumps({"run_id": run_id, "pids": pids}), encoding="utf-8")
        os.replace(tmp, active)

    def poll():
        with open(run_dir / "timeline.jsonl", "a", encoding="utf-8") as tl:
            for name, port in ports.items():
                if name not in handles:
                    continue
                st, code = _get_state(port)
                # alive tells "no answer from a running game" (counts as frozen) from "process gone".
                alive = handles[name].exit_code() is None
                s = Sample(round(time.time() - t0, 1), name, st, code, alive)
                samples.append(s)
                tl.write(json.dumps(asdict(s)) + "\n")

    try:
        servers = [p for p in scn.processes if p.role == "server"]
        others = [p for p in scn.processes if p.role != "server"]
        for spec in servers:
            handles[spec.name] = launcher.start(spec.name, spec.role,
                                                _args(spec, ports[spec.name], run_dir, n_clients))
            save_active()
            while True:
                poll()
                last = [s for s in samples if s.name == spec.name][-1]
                if last.state and last.state.get("listening"):
                    break
                if handles[spec.name].exit_code() is not None or time.time() - t0 > STARTUP_TIMEOUT_S:
                    break
                time.sleep(poll_s)
        for i, spec in enumerate(others):
            if i:
                time.sleep(5 if poll_s >= 1 else 0.5)
            handles[spec.name] = launcher.start(spec.name, spec.role,
                                                _args(spec, ports[spec.name], run_dir, n_clients))
            save_active()

        while True:
            poll()
            elapsed = time.time() - t0
            match_ended = any(
                (run_dir / f"{sp.name}.log").exists()
                and "Match ended" in (run_dir / f"{sp.name}.log").read_text(errors="replace")
                for sp in servers)
            if elapsed >= next_shot[0]:
                take_shots(f"{int(elapsed):04d}s")
                next_shot[0] += screenshot.SHOT_EVERY_S
            o = classify(scn, samples, _records(scn, handles, run_dir, killed), elapsed, match_ended)
            if o.kind != "running":
                take_shots(f"{int(elapsed):04d}s-final")
                break
            time.sleep(poll_s)
    except (RuntimeError, OSError) as e:
        raise HarnessError(str(e)) from e
    finally:
        for n, h in handles.items():
            killed.add(n)  # from here on, exit codes are ours, not the game's
            try:
                h.kill()
            finally:
                h.close()
        active.unlink(missing_ok=True)
        collect_game_dumps(run_dir, t0, config.GAME_LOGS_DIR)

    sig = signature.make(o, scn)
    result = RunResult(run_id, scn.name, o, sig, run_dir, round(time.time() - t0, 1),
                       milestone(samples))
    (run_dir / "result.json").write_text(json.dumps(
        {**asdict(result), "run_dir": str(run_dir)}, indent=2, default=str))
    if o.kind == "pass":
        prune_dumps_for_pass(result)
    return result


def collect_game_dumps(run_dir: Path, since: float, logs_dir: Path) -> list[Path]:
    """Move the game's own crash dumps written during this run into the run folder, so the
    run's dump retention covers them. Dumps from before the run are never touched."""
    moved = []
    try:
        candidates = sorted(logs_dir.glob("POPLAR-*.dmp"))
    except OSError:
        return moved
    for p in candidates:
        try:
            st = p.stat()
            born = min(getattr(st, "st_birthtime", st.st_ctime), st.st_mtime)
            if born < since:
                continue
            dest = run_dir / p.name
            shutil.move(p, dest)
            moved.append(dest)
        except OSError as e:
            print(f"note: could not move game dump {p}: {e}")
    return moved


def prune_dumps_for_pass(r: RunResult) -> None:
    for d in r.run_dir.glob("*.dmp"):
        d.unlink()


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="python -m debugloop.run")
    ap.add_argument("scenario")
    ap.add_argument("--poll", type=float, default=2.0)
    a = ap.parse_args(argv)
    try:
        r = run_scenario(scenario.find_scenario(a.scenario), poll_s=a.poll)
    except (HarnessError, scenario.ScenarioError) as e:
        print(f"HARNESS ERROR: {e}")
        return 2
    print((r.run_dir / "result.json").read_text())
    return 0 if r.outcome.kind == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())

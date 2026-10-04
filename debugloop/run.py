import argparse
import json
import shutil
import sys
import time
import urllib.error
import urllib.request
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path

from . import config, launch, scenario, signature
from .outcome import STARTUP_TIMEOUT_S, Outcome, ProcessRecord, Sample, classify


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


def _get_state(port: int) -> tuple[dict | None, int | None]:
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/state", timeout=4) as r:
            return json.loads(r.read()), r.status
    except urllib.error.HTTPError as e:
        return None, e.code
    except (urllib.error.URLError, TimeoutError, ConnectionError, json.JSONDecodeError):
        return None, None


def _reap_stale(runs_dir: Path) -> None:
    f = runs_dir / "active.json"
    if not f.exists():
        return
    for pid in json.loads(f.read_text()).get("pids", {}).values():
        try:
            launch.ProcessHandle(pid).kill()
        except OSError:
            pass
    f.unlink()


def _preconditions(runs_dir: Path) -> None:
    runs_dir.mkdir(parents=True, exist_ok=True)
    _reap_stale(runs_dir)
    foreign = launch.find_game_processes()
    if foreign:
        raise HarnessError(f"a game process is already running (pids {foreign}); "
                           "close it first - the runner only stops games it started")
    free_mb = shutil.disk_usage(runs_dir).free // (1024 * 1024)
    if free_mb < config.MIN_FREE_MB:
        raise HarnessError(f"only {free_mb} MB free disk space")


def _args(spec, port: int, run_dir: Path, n_clients: int) -> list[str]:
    a = launch.GAME_BASE_ARGS + list(spec.args) + [
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
        crashes = [json.loads(p.read_text()) for p in sorted(run_dir.glob(f"{spec.name}.*.crash.json"))]
        hang_f = run_dir / f"{spec.name}.hang.json"
        log_f = run_dir / f"{spec.name}.log"
        tail = log_f.read_text(errors="replace").splitlines()[-80:] if log_f.exists() else []
        h = handles.get(spec.name)
        recs.append(ProcessRecord(spec.name, spec.role, h.exit_code() if h and spec.name not in killed else None, crashes,
                                  json.loads(hang_f.read_text()) if hang_f.exists() else None, tail))
    return recs


def run_scenario(scn, launcher=None, runs_dir: Path = config.RUNS_DIR, poll_s: float = 2.0) -> RunResult:
    launcher = launcher or launch.RealLauncher()
    _preconditions(runs_dir)
    run_id = f"{datetime.now():%Y%m%d-%H%M%S}-{scn.name}"
    run_dir = runs_dir / run_id
    run_dir.mkdir(parents=True)
    shutil.copy2(scn.path, run_dir / "scenario.toml") if scn.path.exists() else None
    ports = {p.name: config.FIRST_DEBUG_PORT + i for i, p in enumerate(scn.processes)}
    n_clients = sum(p.role == "client" for p in scn.processes)
    handles: dict[str, launch.ProcessHandle] = {}
    killed: set[str] = set()
    samples: list[Sample] = []
    active = runs_dir / "active.json"
    t0 = time.time()

    def save_active():
        active.write_text(json.dumps({"run_id": run_id, "pids": {n: h.pid for n, h in handles.items()}}))

    def poll():
        with open(run_dir / "timeline.jsonl", "a", encoding="utf-8") as tl:
            for name, port in ports.items():
                if name not in handles:
                    continue
                st, code = _get_state(port)
                s = Sample(round(time.time() - t0, 1), name, st, code)
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
            server_log = run_dir / "server.log"
            match_ended = server_log.exists() and "Match ended" in server_log.read_text(errors="replace")
            o = classify(scn, samples, _records(scn, handles, run_dir, killed), elapsed, match_ended)
            if o.kind != "running":
                break
            time.sleep(poll_s)
    except (RuntimeError, OSError) as e:
        raise HarnessError(str(e)) from e
    finally:
        for n, h in handles.items():
            killed.add(n)  # from here on, exit codes are ours, not the game's
            h.kill()
        active.unlink(missing_ok=True)

    sig = signature.make(o, scn)
    result = RunResult(run_id, scn.name, o, sig, run_dir, round(time.time() - t0, 1))
    (run_dir / "result.json").write_text(json.dumps(
        {**asdict(result), "run_dir": str(run_dir)}, indent=2, default=str))
    if o.kind == "pass":
        prune_dumps_for_pass(result)
    return result


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

import json
import subprocess
import sys
from pathlib import Path

import pytest

from debugloop import launch, run, scenario

FAKE = Path(__file__).with_name("fake_game.py")


class FakeLauncher:
    def __init__(self, modes: dict[str, str] | None = None):
        self.modes = modes or {}
        self.started: list[launch.ProcessHandle] = []

    def start(self, name, role, args):
        p = subprocess.Popen([sys.executable, str(FAKE), *args, f"--role={role}",
                              f"--mode={self.modes.get(name, 'pass')}"])
        h = launch.ProcessHandle(p.pid)
        self.started.append(h)
        return h


def scn(limit=6, two=False):
    text = f'name="t"\nstep=1\ntime_limit_s={limit}\nexpect_map="Dojo_P"\n'
    text += '[[process]]\nname="server"\nrole="server"\nargs=[]\n' if two else ""
    text += f'[[process]]\nname="c1"\nrole="{"client" if two else "solo"}"\nargs=[]\n'
    return scenario.parse(text, Path("t.toml"))


@pytest.fixture(autouse=True)
def no_foreign_games(monkeypatch, tmp_path):
    monkeypatch.setattr(launch, "find_game_processes", lambda: [])
    # Never sweep the real game's Logs folder from a test run.
    monkeypatch.setattr(run.config, "GAME_LOGS_DIR", tmp_path / "no-game-logs")


def test_pass(tmp_path):
    L = FakeLauncher()
    r = run.run_scenario(scn(), L, tmp_path, poll_s=0.5)
    assert r.outcome.kind == "pass", r.outcome
    assert (r.run_dir / "result.json").exists()
    assert (r.run_dir / "timeline.jsonl").read_text().strip()
    assert all(h.exit_code() is not None for h in L.started), "runner must stop what it started"
    assert not (tmp_path / "active.json").exists()


def test_full_time_run_is_not_graded_as_exit(tmp_path):
    # The runner kills the fake game at the time limit. That kill's exit code (1) must not
    # be read as the game exiting on its own.
    L = FakeLauncher()
    r = run.run_scenario(scn(), L, tmp_path, poll_s=0.5)
    assert r.outcome.kind == "pass", r.outcome
    assert r.signature is None
    assert [h.exit_code() for h in L.started] == [1], "game was killed by the runner"
    assert json.loads((r.run_dir / "result.json").read_text())["outcome"]["kind"] == "pass"


def test_records_ignore_exit_code_of_harness_killed_process(tmp_path):
    L = FakeLauncher()
    r = run.run_scenario(scn(), L, tmp_path, poll_s=0.5)
    h = L.started[0]
    assert h.exit_code() == 1
    recs = run._records(scn(), {"c1": h}, r.run_dir, killed={"c1"})
    assert recs[0].exit_code is None
    recs = run._records(scn(), {"c1": h}, r.run_dir, killed=set())
    assert recs[0].exit_code == 1


def test_crash_keeps_dump(tmp_path):
    r = run.run_scenario(scn(30), FakeLauncher({"c1": "crash"}), tmp_path, poll_s=0.5)
    assert r.outcome.kind == "crash"
    assert r.signature == "crash:0xc0000005:battleborn+0x1234"
    assert (r.run_dir / "c1.1.dmp").exists()


def test_hang_from_503(tmp_path, monkeypatch):
    monkeypatch.setattr(run.config, "HANG_SECONDS", 1)   # 503s count once the watchdog limit + grace has passed
    r = run.run_scenario(scn(30), FakeLauncher({"c1": "hang"}), tmp_path, poll_s=0.5)
    assert r.outcome.kind == "hang"


def test_server_then_client(tmp_path):
    r = run.run_scenario(scn(8, two=True), FakeLauncher(), tmp_path, poll_s=0.5)
    assert r.outcome.kind == "pass", r.outcome


def test_refuses_when_foreign_game_running(tmp_path, monkeypatch):
    # Injected process finder reports a fake pid; no real game is involved.
    monkeypatch.setattr(launch, "find_game_processes", lambda: [424242])
    L = FakeLauncher()
    with pytest.raises(run.HarnessError, match="already running"):
        run.run_scenario(scn(), L, tmp_path, poll_s=0.5)
    assert L.started == [], "nothing may be started or killed"


def _sleeper():
    return subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])


def _write_active(tmp_path, rec):
    (tmp_path / "active.json").write_text(json.dumps({"run_id": "old", "pids": {"c1": rec}}))


def test_reaps_stale_active_file(tmp_path):
    stale = _sleeper()
    _write_active(tmp_path, run._identity(stale.pid))
    run.run_scenario(scn(), FakeLauncher(), tmp_path, poll_s=0.5)
    assert stale.wait(timeout=10) is not None


def test_does_not_kill_process_with_reused_pid(tmp_path):
    other = _sleeper()
    try:
        rec = run._identity(other.pid)
        rec["create_time"] -= 3600  # same pid, but a different process started it
        _write_active(tmp_path, rec)
        run.run_scenario(scn(), FakeLauncher(), tmp_path, poll_s=0.5)
        assert other.poll() is None, "unrelated process must survive"
        assert not (tmp_path / "active.json").exists()
    finally:
        other.kill()
        other.wait()


def test_does_not_kill_when_exe_name_differs(tmp_path):
    other = _sleeper()
    try:
        rec = run._identity(other.pid)
        rec["exe"] = "Battleborn.exe"
        _write_active(tmp_path, rec)
        run.run_scenario(scn(), FakeLauncher(), tmp_path, poll_s=0.5)
        assert other.poll() is None
    finally:
        other.kill()
        other.wait()


def test_old_format_active_file_kills_nothing(tmp_path):
    other = _sleeper()
    try:
        _write_active(tmp_path, other.pid)  # old format: bare pid
        run.run_scenario(scn(), FakeLauncher(), tmp_path, poll_s=0.5)
        assert other.poll() is None
        assert not (tmp_path / "active.json").exists()
    finally:
        other.kill()
        other.wait()


def test_corrupt_active_file_is_deleted_not_fatal(tmp_path):
    (tmp_path / "active.json").write_text("{not json")
    r = run.run_scenario(scn(), FakeLauncher(), tmp_path, poll_s=0.5)
    assert r.outcome.kind == "pass"
    assert not (tmp_path / "active.json").exists()


def test_same_scenario_twice_in_one_second_gets_distinct_folders(tmp_path, monkeypatch):
    class Frozen(run.datetime):
        @classmethod
        def now(cls, tz=None):
            return run.datetime(2026, 1, 1, 0, 0, 0)
    monkeypatch.setattr(run, "datetime", Frozen)
    a = run.run_scenario(scn(2), FakeLauncher(), tmp_path, poll_s=0.5)
    b = run.run_scenario(scn(2), FakeLauncher(), tmp_path, poll_s=0.5)
    assert a.run_dir != b.run_dir and b.run_dir.exists()


def test_get_state_treats_garbage_as_unreachable(monkeypatch):
    import http.client

    def boom(*a, **k):
        raise http.client.BadStatusLine("x")
    monkeypatch.setattr(run.urllib.request, "urlopen", boom)
    assert run._get_state(1) == (None, None)

    def bad_unicode(*a, **k):
        raise UnicodeDecodeError("utf-8", bytes([255]), 0, 1, "bad")
    monkeypatch.setattr(run.urllib.request, "urlopen", bad_unicode)
    assert run._get_state(1) == (None, None)


def test_unwritable_runs_dir_is_harness_error(tmp_path):
    blocker = tmp_path / "file"
    blocker.write_text("x")
    with pytest.raises(run.HarnessError):
        run.run_scenario(scn(), FakeLauncher(), blocker / "runs", poll_s=0.5)


def test_passing_run_deletes_dumps(tmp_path):
    r = run.run_scenario(scn(), FakeLauncher(), tmp_path, poll_s=0.5)
    (r.run_dir / "x.dmp").write_bytes(b"x")
    run.prune_dumps_for_pass(r)
    assert not list(r.run_dir.glob("*.dmp"))


def test_collect_game_dumps_moves_only_dumps_from_this_run(tmp_path):
    import os, time
    logs, run_dir = tmp_path / "Logs", tmp_path / "run"
    logs.mkdir(); run_dir.mkdir()
    old = logs / "POPLAR-PATCH-87-pc-old.dmp"
    old.write_bytes(b"old")
    os.utime(old, (time.time() - 3600, time.time() - 3600))
    since = time.time() - 1
    new = logs / "POPLAR-PATCH-87-pc-new.dmp"
    new.write_bytes(b"new")
    other = logs / "PCLaunch.log"
    other.write_text("log")
    moved = run.collect_game_dumps(run_dir, since, logs)
    assert moved == [run_dir / new.name]
    assert (run_dir / new.name).read_bytes() == b"new" and not new.exists()
    assert old.exists() and other.exists()


def test_collect_game_dumps_missing_logs_dir_is_fine(tmp_path):
    assert run.collect_game_dumps(tmp_path, 0, tmp_path / "nope") == []


def test_run_sweeps_game_dumps_into_run_folder(tmp_path, monkeypatch):
    logs = tmp_path / "Logs"
    logs.mkdir()
    monkeypatch.setattr(run.config, "GAME_LOGS_DIR", logs)
    real_start = FakeLauncher.start

    def start_and_dump(self, name, role, args):
        h = real_start(self, name, role, args)
        (logs / "POPLAR-x.dmp").write_bytes(b"d")
        return h

    monkeypatch.setattr(FakeLauncher, "start", start_and_dump)
    r = run.run_scenario(scn(30), FakeLauncher({"c1": "crash"}), tmp_path / "runs", poll_s=0.5)
    assert (r.run_dir / "POPLAR-x.dmp").exists() and not (logs / "POPLAR-x.dmp").exists()


def test_freeze_at_the_time_limit_runs_over_and_ends_as_hang(tmp_path, monkeypatch):
    # The fake game stops answering after 2 s; the time limit is 4 s. The runner must not stop
    # with a pass at 4 s, but keep sampling (up to watchdog limit + grace) and grade a hang.
    monkeypatch.setattr(run.config, "HANG_SECONDS", 1)
    r = run.run_scenario(scn(4), FakeLauncher({"c1": "hang"}), tmp_path, poll_s=0.5)
    assert r.outcome.kind == "hang"
    assert r.elapsed_s > 4


def test_timeline_records_whether_the_process_was_alive(tmp_path):
    r = run.run_scenario(scn(4), FakeLauncher({"c1": "exit"}), tmp_path, poll_s=0.5)
    rows = [json.loads(l) for l in (r.run_dir / "timeline.jsonl").read_text().splitlines()]
    assert rows[0]["alive"] is True
    assert all("alive" in row for row in rows)


def test_multiplayer_args_save_memory_solo_keeps_its_resolution(tmp_path):
    # Three instances do not fit in this laptop's free RAM at 960x540 (spike S3): the server
    # renders nothing and clients run at 640x360. Solo is a single instance and stays as it was.
    s = scn(two=True)
    server = run._args(s.processes[0], 18080, tmp_path, 1)
    client = run._args(s.processes[1], 18081, tmp_path, 1)
    solo = run._args(scn().processes[0], 18080, tmp_path, 0)
    assert "-nullrhi" in server and "-nullrhi" not in client and "-nullrhi" not in solo
    assert "-ResX=640" in client and "-ResY=360" in client
    assert "-ResX=960" not in client and "-ResY=540" not in client
    assert "-ResX=960" in solo and "-ResY=540" in solo

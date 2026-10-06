import json
import subprocess
import sys
import time
from pathlib import Path

import psutil
import pytest

from debugloop import config, launch, playsession, run, scenario

FAKE = Path(__file__).with_name("fake_game.py")


class FakeLauncher:
    def __init__(self):
        self.started = []

    def start(self, name, role, args):
        p = subprocess.Popen([sys.executable, str(FAKE), *args, f"--role={role}", "--mode=pass"])
        h = launch.ProcessHandle(p.pid)
        self.started.append(h)
        return h


def scn():
    text = ('name="p"\nstep=1\ntime_limit_s=60\nexpect_map="Dojo_P"\n'
            '[[process]]\nname="server"\nrole="server"\nargs=[]\n'
            '[[process]]\nname="c1"\nrole="client"\nargs=["-rbautopilot"]\n')
    return scenario.parse(text, Path("p.toml"))


@pytest.fixture(autouse=True)
def isolated(monkeypatch, tmp_path):
    monkeypatch.setattr(launch, "find_game_processes", lambda: [])
    monkeypatch.setattr(run.config, "GAME_LOGS_DIR", tmp_path / "no-game-logs")
    # Away from the real debug ports, so a live game on this machine never collides with the test.
    monkeypatch.setattr(config, "FIRST_DEBUG_PORT", 18190)


def start(tmp_path, L):
    return playsession.start(scn(), launcher=L, runs_dir=tmp_path / "runs", state_dir=tmp_path / "state",
                             poll_s=0.2, wait_s=15)


def test_start_leaves_processes_running_and_writes_play_json(tmp_path):
    L = FakeLauncher()
    info = start(tmp_path, L)
    try:
        assert info["ports"] == {"server": 18190, "c1": 18191}
        assert all(psutil.pid_exists(h.pid) for h in L.started)
        saved = json.loads((tmp_path / "state" / "play.json").read_text())
        assert saved["ports"] == info["ports"] and set(saved["pids"]) == {"server", "c1"}
        assert not (tmp_path / "runs" / "active.json").exists()   # a play session is not a graded run
        assert playsession.alive(tmp_path / "state")["scenario"] == "p"
    finally:
        playsession.stop(tmp_path / "state")


def test_stop_kills_and_forgets(tmp_path):
    L = FakeLauncher()
    start(tmp_path, L)
    pids = [h.pid for h in L.started]
    assert sorted(playsession.stop(tmp_path / "state")) == ["c1", "server"]
    deadline = time.time() + 5
    while time.time() < deadline and any(psutil.pid_exists(p) for p in pids):
        time.sleep(0.1)
    assert not any(psutil.pid_exists(p) for p in pids)
    assert playsession.alive(tmp_path / "state") is None
    assert playsession.stop(tmp_path / "state") == []


def test_alive_is_none_for_stale_file(tmp_path):
    (tmp_path / "state").mkdir()
    (tmp_path / "state" / "play.json").write_text(json.dumps(
        {"run_dir": "x", "ports": {"c1": 18080}, "pids": {"c1": {"pid": 1, "create_time": 0, "exe": "nope"}}}))
    assert playsession.alive(tmp_path / "state") is None


def test_alive_is_none_for_unreadable_file(tmp_path):
    (tmp_path / "state").mkdir()
    (tmp_path / "state" / "play.json").write_text("{half")
    assert playsession.alive(tmp_path / "state") is None


def test_start_refuses_while_a_session_is_alive(tmp_path):
    L = FakeLauncher()
    start(tmp_path, L)
    try:
        with pytest.raises(run.HarnessError, match="play session is already running"):
            start(tmp_path, FakeLauncher())
    finally:
        playsession.stop(tmp_path / "state")

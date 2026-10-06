import io
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

import pytest

from debugloop import play

FAKE = Path(__file__).with_name("fake_game.py")
PORTS = range(18180, 18182)   # outside the harness's 18080+ range so a live game never collides


@pytest.fixture
def fake(tmp_path):
    """Start fake games; each call returns the Instance the play module should find for it."""
    procs = []

    def start(port=18180, mode="pass", name="c1"):
        p = subprocess.Popen([sys.executable, str(FAKE), f"-rbinstance={name}", f"-rbdebugport={port}",
                              f"-rbrundir={tmp_path}", "--role=client", f"--mode={mode}"])
        procs.append(p)
        for _ in range(100):
            try:
                urllib.request.urlopen(f"http://127.0.0.1:{port}/ping", timeout=0.2)
                break
            except OSError:
                time.sleep(0.05)
        return play.Instance(name, "client", port, p.pid)

    yield start
    for p in procs:
        p.kill()


def test_discover_finds_the_fake(fake):
    inst = fake()
    found = play.discover(ports=PORTS)
    assert [(i.name, i.role, i.port, i.pid) for i in found] == [("c1", "client", 18180, inst.pid)]


def test_discover_finds_nothing_when_nothing_runs():
    assert play.discover(ports=PORTS) == []


def test_resolve_by_name_and_port(fake):
    inst = fake()
    assert play.resolve("c1", [inst]).port == 18180
    assert play.resolve(18180, [inst]).name == "c1"
    with pytest.raises(play.PlayError, match="no instance 'c9'; running: c1 \\(18180\\)"):
        play.resolve("c9", [inst])


def test_order_round_trips(fake):
    inst = fake()
    j = play.order(inst, mode="hold", fire=False)
    assert j["mode"] == "hold" and j["order"]["fire"] is False
    assert play.combat(inst)["order"]["mode"] == "hold"


def test_order_sends_only_given_fields(fake):
    inst = fake()
    play.order(inst, mode="hold")
    j = play.order(inst, fire=False)
    assert j["order"]["mode"] == "hold"


def test_order_rejects_unknown_mode(fake):
    with pytest.raises(play.PlayError, match="c1: unknown_mode"):
        play.order(fake(), mode="dance")


def test_act_without_pawn_is_a_plain_error(fake):
    with pytest.raises(play.PlayError, match="c1: no_pawn"):
        play.act(fake(mode="dead"), "jump")


def test_act_ok(fake):
    assert play.act(fake(), "jump") == {"ok": True, "action": "jump"}


def test_no_answer_is_a_play_error():
    with pytest.raises(play.PlayError, match="no answer from port 18181"):
        play.state(play.Instance("c2", "client", 18181, 0))


def test_exec_and_log(fake):
    inst = fake()
    assert play.exec_(inst, "Jump") == "ok"
    assert play.log(inst, lines=1) == "[COMBAT] target 7f0001"


def test_situation_text_is_short_and_names_the_target(fake):
    text = play.situation_text(fake())
    lines = text.splitlines()
    assert len(lines) <= 20
    assert lines[0] == "c1: Rath hp 800/1000 at (10, 20, 30) yaw 90"
    assert "mode hunt  target 7f0001" in text
    assert "* 7f0001 Thrall minion 640u bearing -12 visible hp 300" in text
    assert "- 7f0002 OscarMike bot 2100u bearing 95 hidden hp 900" in text
    assert "skills: 1 ready, 2 in 4.5s, 3 in 60.0s" in text
    assert "stats: shots 3 skills 1 kills 0 deaths 1 dmg taken 200" in text


def test_look_without_window_returns_none(fake, monkeypatch):
    monkeypatch.setattr(play.screenshot, "capture", lambda pid, path: False)
    assert play.look(fake()) is None


def test_look_resizes_png(fake, monkeypatch):
    from PIL import Image

    def fake_capture(pid, path):
        Image.new("RGB", (960, 540), "red").save(path, "JPEG")
        return True

    monkeypatch.setattr(play.screenshot, "capture", fake_capture)
    img = Image.open(io.BytesIO(play.look(fake(), max_width=240)))
    assert img.format == "PNG" and img.size == (240, 135)


def test_cli_list_and_situation(fake, capsys):
    fake()
    assert play.main(["--ports", "18180-18181", "list"]) == 0
    assert "c1" in capsys.readouterr().out
    assert play.main(["--ports", "18180-18181", "situation", "c1"]) == 0
    assert "Thrall" in capsys.readouterr().out


def test_cli_error_is_one_line(fake, capsys):
    fake(mode="dead")
    assert play.main(["--ports", "18180-18181", "act", "c1", "jump"]) == 1
    assert capsys.readouterr().out.strip() == "error: c1: no_pawn"


def test_capture_refuses_a_minimised_window(monkeypatch, tmp_path):
    """Windows reports a minimised window at a tiny off-screen size; that is no picture, not a black one."""
    monkeypatch.setattr(play.screenshot, "_main_window", lambda pid: 1234)
    monkeypatch.setattr(play.screenshot, "_is_minimised", lambda hwnd: True)
    assert play.screenshot.capture(99, tmp_path / "x.jpg") is False
    assert not (tmp_path / "x.jpg").exists()

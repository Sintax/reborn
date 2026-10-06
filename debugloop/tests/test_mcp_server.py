import asyncio
import base64

import pytest

from debugloop import mcp_server, play


class FakePlay:
    """Stands in for debugloop.play inside the tool functions."""

    PlayError = play.PlayError

    def __init__(self):
        self.inst = play.Instance("c1", "client", 18080, 123)
        self.calls = []
        self.picture = None

    def discover(self, *a, **k):
        return [self.inst]

    def resolve(self, which, instances=None):
        if which != "c1":
            raise play.PlayError(f"no instance '{which}'; running: c1 (18080)")
        return self.inst

    def situation_text(self, inst):
        return "c1: Rath hp 800/1000"

    def combat(self, inst):
        return {"mode": "hunt"}

    def state(self, inst):
        return {"map": "Dojo_P"}

    def order(self, inst, **kw):
        self.calls.append(("order", kw))
        return {"order": {"mode": kw.get("mode") or "hunt"}}

    def act(self, inst, action, duration_s=None):
        if action == "teabag":
            raise play.PlayError("c1: unknown_action")
        return {"ok": True, "action": action}

    def exec_(self, inst, command):
        return "ok"

    def log(self, inst, lines=40):
        return "[GAME] hi"

    def look(self, inst, max_width=480):
        return self.picture


@pytest.fixture
def fp(monkeypatch):
    f = FakePlay()
    monkeypatch.setattr(mcp_server, "play", f)
    return f


def test_bb_list(fp):
    out = mcp_server.bb_list()
    assert out == "c1 (client) port 18080 pid 123"


def test_bb_list_when_nothing_runs(fp, monkeypatch):
    monkeypatch.setattr(fp, "discover", lambda *a, **k: [])
    assert "no test games running" in mcp_server.bb_list()


def test_bb_situation(fp):
    assert mcp_server.bb_situation("c1") == "c1: Rath hp 800/1000"


def test_unknown_instance_is_text_not_exception(fp):
    out = mcp_server.bb_situation("c9")
    assert out == "error: no instance 'c9'; running: c1 (18080)"


def test_bb_order_passes_fields(fp):
    mcp_server.bb_order("c1", mode="goto", point=[1, 2, 3], fire=False)
    assert fp.calls == [("order", {"mode": "goto", "target": None, "point": [1, 2, 3], "fire": False,
                                   "skills": None, "enabled": None})]


def test_bb_act_error_is_text(fp):
    assert mcp_server.bb_act("c1", "teabag") == "error: c1: unknown_action"


def test_bb_look_without_window_is_text(fp):
    assert mcp_server.bb_look("c1") == "c1: no window to capture (is the game minimised or gone?)"


def test_bb_look_returns_png_image(fp):
    fp.picture = b"\x89PNG fake"
    content = mcp_server.bb_look("c1").to_image_content()
    assert content.mime_type == "image/png" and base64.b64decode(content.data) == b"\x89PNG fake"


def test_server_registers_every_tool():
    names = {t.name for t in asyncio.run(mcp_server.build_server().list_tools())}
    assert names == {"bb_list", "bb_situation", "bb_combat", "bb_state", "bb_look", "bb_order", "bb_act",
                     "bb_exec", "bb_log"}

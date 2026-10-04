import json
from pathlib import Path

from debugloop import analyze
from debugloop.outcome import Outcome
from debugloop.run import RunResult


def test_known_addresses_parses_hook_table(tmp_path):
    (tmp_path / "Init.cpp").write_text(
        "Hooks::PoplarGameInfoSetup = safetyhook::create_inline((void*)(Globals::baseAddress + 0x1474140), &Hooks::PoplarGameInfoSetupHook);\n")
    assert analyze.known_addresses(tmp_path) == {0x1474140: "PoplarGameInfoSetupHook"}


def test_known_addresses_real_repo_has_tick_hook():
    assert "GameEngineTickHook" in analyze.known_addresses().values()


def test_describe_frame():
    funcs = [(0x1474140, 0x1474400, "FUN_141474140")]
    hooks = {0x1474140: "PoplarGameInfoSetupHook"}
    d = analyze.describe_frame("battleborn+0x1474150", funcs, hooks)
    assert "FUN_141474140" in d and "PoplarGameInfoSetupHook" in d
    assert analyze.describe_frame("reborn+0x10", funcs, hooks) == "reborn+0x10"


def test_write_triage_without_tools(tmp_path, monkeypatch):
    monkeypatch.setattr(analyze, "find_cdb", lambda: None)
    (tmp_path / "c1.1.crash.json").write_text(json.dumps(
        {"code": "0xC0000005", "frames": ["battleborn+0x10"], "dump": "c1.1.dmp"}))
    (tmp_path / "c1.log").write_text("line a\nline b\n")
    (tmp_path / "timeline.jsonl").write_text('{"t": 1, "name": "c1", "state": {"ticks": 3}, "http_status": 200}\n')
    r = RunResult("rid", "s1", Outcome("crash", "", "c1", "playing", "battleborn+0x10", "0xC0000005"),
                  "crash:0xc0000005:battleborn+0x10", tmp_path, 12.0)
    text = analyze.write_triage(r).read_text()
    assert "crash:0xc0000005:battleborn+0x10" in text
    assert "line b" in text
    assert "cdb not found" in text

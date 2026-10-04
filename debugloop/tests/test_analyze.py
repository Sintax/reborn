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
    (tmp_path / "c1.1.dmp").write_bytes(b"MDMP")
    (tmp_path / "c1.log").write_text("line a\nline b\n")
    (tmp_path / "timeline.jsonl").write_text('{"t": 1, "name": "c1", "state": {"ticks": 3}, "http_status": 200}\n')
    r = RunResult("rid", "s1", Outcome("crash", "", "c1", "playing", "battleborn+0x10", "0xC0000005"),
                  "crash:0xc0000005:battleborn+0x10", tmp_path, 12.0)
    text = analyze.write_triage(r).read_text()
    assert "crash:0xc0000005:battleborn+0x10" in text
    assert "line b" in text
    assert "cdb not found" in text


def _result(tmp_path):
    return RunResult("rid", "s1", Outcome("crash", "", "c1", "playing", "x", "0x1"),
                     "sig", tmp_path, 1.0)


def test_describe_frame_unparseable():
    for f in ("battleborn", "battleborn+", "battleborn+0x1G", "battleborn+<unknown>"):
        assert analyze.describe_frame(f, [], {}) == f


def test_load_functions_corrupt(tmp_path):
    p = tmp_path / "f.json"
    p.write_text('[{"name": "a", "start": 1')
    assert analyze.load_functions(p) == []
    p.write_text('[{"name": "a"}]')
    assert analyze.load_functions(p) == []


def test_write_triage_unreadable_report(tmp_path, monkeypatch):
    monkeypatch.setattr(analyze, "find_cdb", lambda: None)
    (tmp_path / "bad.crash.json").write_text('{"code": "0x1", "frames": [')
    (tmp_path / "odd.hang.json").write_text('[1, 2]')
    text = analyze.write_triage(_result(tmp_path)).read_text()
    assert "(unreadable report: bad.crash.json)" in text
    assert "(unreadable report: odd.hang.json)" in text


def test_write_triage_no_cdb_note_without_dump(tmp_path, monkeypatch):
    monkeypatch.setattr(analyze, "find_cdb", lambda: None)
    (tmp_path / "c.crash.json").write_text(json.dumps({"code": "0x1", "frames": [], "dump": "gone.dmp"}))
    text = analyze.write_triage(_result(tmp_path)).read_text()
    assert "(no dump)" in text and "cdb not found" not in text


def test_known_addresses_skips_comments(tmp_path):
    (tmp_path / "a.cpp").write_text(
        "  // x = create_inline((void*)(Globals::baseAddress + 0x10), &Hooks::OldHook);\n"
        "y = create_inline((void*)(Globals::baseAddress + 0x20), &Hooks::NewHook);\n")
    assert analyze.known_addresses(tmp_path) == {0x20: "NewHook"}

def test_cdb_symbol_path_is_an_argument_not_a_command(tmp_path, monkeypatch):
    # Live run 2026-10-04: ".symfix+ <cache>; ..." in -c made cdb take the rest of the command
    # line as the cache folder, so Windows symbols never loaded ("OS symbols are WRONG").
    seen = {}

    class P:
        stdout = "ok"

    def fake_run(args, **kw):
        seen["args"] = args
        return P()

    monkeypatch.setattr(analyze.subprocess, "run", fake_run)
    analyze.run_cdb(tmp_path / "x.dmp", cdb=tmp_path / "cdb.exe")
    args = seen["args"]
    y = args[args.index("-y") + 1]
    assert y.lower().startswith("srv*") and "msdl.microsoft.com" in y
    assert y.endswith(";" + str(analyze.config.WIN64))
    c = args[args.index("-c") + 1]
    assert ".symfix" not in c and ".sympath" not in c
    assert "!analyze -v" in c and c.rstrip().endswith("q")

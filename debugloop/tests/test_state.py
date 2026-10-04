import pytest
from debugloop import state

def test_roundtrip(tmp_path):
    s = state.LoopState(step=2, consecutive_passes=1, current_bug="exit:3")
    s.save(tmp_path)
    assert state.LoopState.load(tmp_path) == s

def test_missing_gives_defaults(tmp_path):
    assert state.LoopState.load(tmp_path) == state.LoopState()

def test_corrupt_raises_not_resets(tmp_path):
    (tmp_path / "loop_state.json").write_text("{half")
    with pytest.raises(state.StateCorrupt):
        state.LoopState.load(tmp_path)

def test_atomic_write_leaves_no_temp(tmp_path):
    state.atomic_write(tmp_path / "a.json", "{}")
    assert [p.name for p in tmp_path.iterdir()] == ["a.json"]

def test_bad_utf8_is_corrupt(tmp_path):
    (tmp_path / "loop_state.json").write_bytes(b'{"step": "\xff\xfe"}')
    with pytest.raises(state.StateCorrupt):
        state.LoopState.load(tmp_path)

def test_old_format_state_file_loads_with_defaults(tmp_path):
    # The live state.json written before bug_head existed (join bug open at attempt 2 of 5).
    (tmp_path / "loop_state.json").write_text(
        '{"step": 1, "consecutive_passes": 0, "scenario_index": 0, "current_bug": "timeout:startup",'
        ' "bug_scenario": "s1-dojo-1client", "attempts_on_current": 1, "harness_errors_in_row": 0,'
        ' "stopped_reason": null, "bug_phase": "startup", "bug_elapsed_s": 241.8}')
    s = state.LoopState.load(tmp_path)
    assert s.current_bug == "timeout:startup" and s.attempts_on_current == 1
    assert s.bug_head is None

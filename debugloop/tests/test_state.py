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

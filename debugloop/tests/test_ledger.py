from debugloop import ledger

def test_record_counts_and_persists(tmp_path):
    L = ledger.Ledger.load(tmp_path)
    L.record("exit:3", "s1", "run1")
    b = L.record("exit:3", "s2", "run2")
    assert b.count == 2 and b.scenarios == ["s1", "s2"] and b.status == "open"
    L.save()
    L2 = ledger.Ledger.load(tmp_path)
    assert L2.get("exit:3").runs == ["run1", "run2"]
    assert "exit:3" in (tmp_path / "ledger.md").read_text()

def test_fixed_bug_reopens_on_recurrence(tmp_path):
    L = ledger.Ledger.load(tmp_path)
    L.record("exit:3", "s1", "r1")
    L.set_status("exit:3", "fixed", "patched X")
    b = L.record("exit:3", "s1", "r2")
    assert b.status == "open" and any("reopened" in n for n in b.notes)

def test_open_bugs_sorted_by_count(tmp_path):
    L = ledger.Ledger.load(tmp_path)
    L.record("a", "s", "r"); L.record("b", "s", "r"); L.record("b", "s", "r2")
    assert [b.signature for b in L.open_bugs()] == ["b", "a"]

import pytest
from debugloop.state import StateCorrupt

@pytest.mark.parametrize("content", [b"{half", b'{"a": "\xff"}', b"[1, 2]", b'{"a": {"nope": 1}}'])
def test_corrupt_ledger_raises_not_resets(tmp_path, content):
    (tmp_path / "ledger.json").write_bytes(content)
    with pytest.raises(StateCorrupt):
        ledger.Ledger.load(tmp_path)
    assert (tmp_path / "ledger.json").read_bytes() == content

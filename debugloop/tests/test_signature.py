from debugloop import signature
from debugloop.outcome import Outcome
from debugloop.tests.test_outcome import scn

def O(kind, **kw):
    d = dict(detail="", process="c1", phase="playing", frame=None, code=None); d.update(kw)
    return Outcome(kind, **d)

def test_crash_skips_system_frames_and_normalizes_server():
    o = O("crash", code="0xC0000005", frame="NTDLL+0x1|SERVERBORN+0x1A2B3C")
    assert signature.make(o, scn()) == "crash:0xc0000005:battleborn+0x1a2b3c"

def test_reborn_frame_rounded_and_paired():
    o = O("crash", code="0xC0000005", frame="reborn+0x4567|battleborn+0x99")
    assert signature.make(o, scn()) == "crash:0xc0000005:reborn+0x4500>battleborn+0x99"

def test_other_kinds():
    assert signature.make(O("exit", code="3"), scn()) == "exit:3"
    assert signature.make(O("disconnect", detail="client:timeout"), scn()) == "disconnect:client:timeout"
    assert signature.make(O("desync"), scn()) == "desync:Dojo_P"
    assert signature.make(O("timeout", phase="startup"), scn()) == "timeout:startup"
    assert signature.make(O("pass"), scn()) is None

def test_slug_is_filename_safe():
    assert signature.slug("crash:0xc0000005:reborn+0x4500>battleborn+0x99") == \
        "crash_0xc0000005_reborn+0x4500_battleborn+0x99"

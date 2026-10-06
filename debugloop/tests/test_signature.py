from debugloop import signature
from debugloop.outcome import Outcome
from debugloop.tests.test_outcome import scn

def O(kind, **kw):
    d = dict(detail="", process="c1", phase="playing", frame=None, code=None); d.update(kw)
    return Outcome(kind, **d)

def test_crash_skips_system_frames_and_normalizes_server():
    o = O("crash", code="0xC0000005", frame="NTDLL+0x1|SERVERBORN+0x1A2B3C")
    assert signature.make(o, scn()) == "crash:0xc0000005:battleborn+0x1a2b3c"

def test_reborn_frame_paired_with_battleborn():
    o = O("crash", code="0xC0000005", frame="reborn+0x4567|battleborn+0x99")
    assert signature.make(o, scn()) == "crash:0xc0000005:reborn>battleborn+0x99"

def test_reborn_frame_different_offsets_same_battleborn():
    o1 = O("crash", code="0xC0000005", frame="reborn+0x1234|battleborn+0x99")
    o2 = O("crash", code="0xC0000005", frame="reborn+0x9F00|battleborn+0x99")
    assert signature.make(o1, scn()) == signature.make(o2, scn())

def test_reborn_frame_alone_is_rounded():
    o = O("crash", code="0xC0000005", frame="reborn+0x4567")
    assert signature.make(o, scn()) == "crash:0xc0000005:reborn+0x4500"

def test_other_kinds():
    assert signature.make(O("exit", code="3"), scn()) == "exit:3"
    assert signature.make(O("disconnect", detail="client:timeout"), scn()) == "disconnect:client:timeout"
    assert signature.make(O("desync"), scn()) == "desync:Dojo_P"
    assert signature.make(O("invisible"), scn()) == "invisible:Dojo_P"
    assert signature.make(O("timeout", phase="startup"), scn()) == "timeout:startup"
    assert signature.make(O("pass"), scn()) is None

def test_slug_is_filename_safe():
    assert signature.slug("crash:0xc0000005:reborn>battleborn+0x99") == \
        "crash_0xc0000005_reborn_battleborn+0x99"

def test_windows_ui_and_network_modules_are_skipped():
    for mod in ("user32", "win32u", "gdi32", "d3d11", "dxgi", "ws2_32", "mswsock", "combase"):
        o = O("crash", code="0xC0000005", frame=f"{mod.upper()}+0x10|battleborn+0x99")
        assert signature.make(o, scn()) == "crash:0xc0000005:battleborn+0x99", mod

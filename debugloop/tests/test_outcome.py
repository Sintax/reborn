from pathlib import Path
from debugloop import outcome, scenario
from debugloop.outcome import Sample, ProcessRecord

def scn(pass_when="survive", limit=900, expect_map="Dojo_P"):
    text = f'''
name="t"
step=1
time_limit_s={limit}
pass_when="{pass_when}"
expect_map="{expect_map}"
[[process]]
name="server"
role="server"
args=[]
[[process]]
name="c1"
role="client"
args=[]
'''
    return scenario.parse(text, Path("t.toml"))

def srv(**kw):
    s = {"ticks": 10, "listening": True, "map": "Dojo_P", "connections": 1,
         "player_locations": {"c1": [0, 0, 0]}}
    s.update(kw); return s

def cli(**kw):
    s = {"ticks": 10, "has_pawn": True, "connected": True, "map": "Dojo_P",
         "pawn_location": [0, 0, 0]}
    s.update(kw); return s

def recs(**over):
    base = {"server": ProcessRecord("server", "server", None, [], None, []),
            "c1": ProcessRecord("c1", "client", None, [], None, [])}
    base.update(over); return list(base.values())

def pair(t, s=None, c=None):
    return [Sample(t, "server", s or srv(), 200), Sample(t, "c1", c or cli(), 200)]

def test_survive_to_time_limit_passes():
    o = outcome.classify(scn(), pair(1), recs(), 900, False)
    assert o.kind == "pass"

def test_clean_exit_mid_run_is_not_pass():
    r = recs(server=ProcessRecord("server", "server", 0, [], None, []))
    o = outcome.classify(scn(), pair(1), r, 100, False)
    assert o.kind == "exit" and o.code == "0"

def test_match_end_exit_zero_passes():
    r = recs(server=ProcessRecord("server", "server", 0, [], None, []))
    o = outcome.classify(scn("match_end"), pair(1), r, 100, True)
    assert o.kind == "pass"

def test_crash_wins_over_exit():
    rep = {"code": "0xC0000005", "frames": ["battleborn+0x10"], "first_chance": True}
    r = recs(c1=ProcessRecord("c1", "client", 3, [rep], None, []))
    o = outcome.classify(scn(), pair(1), r, 50, False)
    assert o.kind == "crash" and o.process == "c1" and o.code == "0xC0000005"

def test_first_chance_report_on_live_process_is_ignored():
    rep = {"code": "0xC0000005", "frames": ["battleborn+0x10"], "first_chance": True}
    r = recs(c1=ProcessRecord("c1", "client", None, [rep], None, []))
    assert outcome.classify(scn(), pair(1), r, 50, False).kind == "running"

def test_three_503s_is_hang():
    s = pair(1) + [Sample(t, "c1", None, 503) for t in (3, 5, 7)]
    o = outcome.classify(scn(), s, recs(), 60, False)
    assert o.kind == "hang" and o.frame == "unknown"

def test_client_disconnect_in_play():
    s = pair(1) + pair(3, c=cli(connected=False, disconnect_reason="timeout"))
    o = outcome.classify(scn(), s, recs(), 60, False)
    assert (o.kind, o.detail) == ("disconnect", "client:timeout")

def test_desync_needs_five_samples():
    far = cli(pawn_location=[2000, 0, 0])
    s = pair(1) + [x for t in range(4) for x in pair(3 + 2 * t, c=far)]
    assert outcome.classify(scn(), s, recs(), 60, False).kind == "running"
    s += pair(11, c=far)
    assert outcome.classify(scn(), s, recs(), 60, False).kind == "desync"

def test_startup_timeout():
    s = [Sample(1, "server", srv(), 200), Sample(1, "c1", cli(has_pawn=False), 200)]
    o = outcome.classify(scn(), s, recs(), 241, False)
    assert (o.kind, o.phase) == ("timeout", "startup")

def test_wrong_map():
    o = outcome.classify(scn(), pair(1, c=cli(map="Frontend")), recs(), 60, False)
    assert o.kind == "exit" and o.code == "wrong_map"

def test_first_chance_on_exit_zero_after_match_end():
    rep = {"code": "0xC0000005", "frames": ["battleborn+0x10"], "first_chance": True}
    r = recs(server=ProcessRecord("server", "server", 0, [rep], None, []))
    o = outcome.classify(scn("match_end"), pair(1), r, 100, True)
    assert o.kind == "pass"

def test_mid_run_exit_zero_with_first_chance():
    rep = {"code": "0xC0000005", "frames": ["battleborn+0x10"], "first_chance": True}
    r = recs(server=ProcessRecord("server", "server", 0, [rep], None, []))
    o = outcome.classify(scn(), pair(1), r, 100, False)
    assert o.kind == "exit" and o.code == "0"

def test_exit_pass_priority_with_client_first():
    # Both client and server exit 0 at match end; expect pass.
    # With client listed first, verifies we check non-zero exits first (not in order).
    r_client_first = [
        ProcessRecord("c1", "client", 0, [], None, []),
        ProcessRecord("server", "server", 0, [], None, [])
    ]
    o = outcome.classify(scn("match_end"), pair(1), r_client_first, 100, True)
    assert o.kind == "pass"

def test_server_pass_blocked_by_client_non_zero_exit():
    # Client exits -1, server exits 0 at match end; expect exit:-1 (not pass).
    # Non-zero client exit blocks server's pass, regardless of order.
    r = [
        ProcessRecord("c1", "client", -1, [], None, []),
        ProcessRecord("server", "server", 0, [], None, [])
    ]
    o = outcome.classify(scn("match_end"), pair(1), r, 100, True)
    assert o.kind == "exit" and o.code == "-1"

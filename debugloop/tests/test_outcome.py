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

def test_second_chance_report_preferred_over_first_chance():
    first = {"code": "0xC0000005", "address": "battleborn+0x10", "frames": ["battleborn+0x10"],
             "first_chance": True}
    last = {"code": "0xC0000374", "address": "ntdll+0x99", "frames": ["ntdll+0x99"],
            "first_chance": False}
    r = recs(c1=ProcessRecord("c1", "client", 3, [first, last], None, []))
    o = outcome.classify(scn(), pair(1), r, 50, False)
    assert o.kind == "crash" and o.frame == "ntdll+0x99" and o.code == "0xC0000374"

def test_first_chance_report_on_live_process_is_ignored():
    rep = {"code": "0xC0000005", "frames": ["battleborn+0x10"], "first_chance": True}
    r = recs(c1=ProcessRecord("c1", "client", None, [rep], None, []))
    assert outcome.classify(scn(), pair(1), r, 50, False).kind == "running"

def test_game_thread_fault_then_unresponsive_is_crash():
    # Live run 2026-10-04: the game's own handler catches the fault, logs "Critical error" and
    # sits there, so no final report comes and the process never exits.
    rep = {"code": "0xC0000005", "address": "reborn+0xd84d4",
           "frames": ["reborn+0xd84d4", "battleborn+0xee67af"], "game_thread": True, "first_chance": True}
    r = recs(c1=ProcessRecord("c1", "client", None, [rep], None, []))
    s = pair(1) + [Sample(t, "c1", None, 503) for t in (3, 5, 7)]
    o = outcome.classify(scn(), s, r, 60, False)
    assert (o.kind, o.process, o.code, o.phase) == ("crash", "c1", "0xC0000005", "playing")
    assert o.frame == "reborn+0xd84d4|battleborn+0xee67af"

def test_game_thread_fault_then_hang_report_is_crash():
    rep = {"code": "0xC0000005", "frames": ["battleborn+0x10"], "game_thread": True, "first_chance": True}
    r = recs(c1=ProcessRecord("c1", "client", None, [rep], {"seconds": 60, "frames": []}, []))
    assert outcome.classify(scn(), pair(1), r, 60, False).kind == "crash"

def test_other_thread_fault_then_unresponsive_stays_hang():
    rep = {"code": "0xC0000005", "frames": ["battleborn+0x10"], "game_thread": False, "first_chance": True}
    r = recs(c1=ProcessRecord("c1", "client", None, [rep], None, []))
    s = pair(1) + [Sample(t, "c1", None, 503) for t in range(3, 3 + 72, 4)]
    assert outcome.classify(scn(), s, r, 80, False).kind == "hang"

def test_503s_past_the_watchdog_limit_is_hang():
    s = pair(1) + [Sample(t, "c1", None, 503) for t in range(3, 3 + 72, 4)]
    o = outcome.classify(scn(), s, recs(), 80, False)
    assert o.kind == "hang" and o.frame == "unknown"

def test_503s_wait_for_the_watchdog_report():
    # Live run 2026-10-04: with -rbhangsecs=15 the runner gave up after 15 s of 503s and killed
    # the game a moment before the watchdog wrote its hang report.
    s = pair(1) + [Sample(t, "c1", None, 503) for t in (6, 11, 16)]
    assert outcome.classify(scn(), s, recs(), 20, False).kind == "running"

def test_watchdog_limit_comes_from_rbhangsecs():
    sc = scn()
    sc.processes[1].args = ["-rbhangsecs=15"]
    s = pair(1) + [Sample(t, "c1", None, 503) for t in range(6, 6 + 28, 4)]
    assert outcome.classify(sc, s, recs(), 40, False).kind == "hang"
    s = pair(1) + [Sample(t, "c1", None, 503) for t in range(6, 6 + 16, 4)]
    assert outcome.classify(sc, s, recs(), 30, False).kind == "running"

def test_503s_before_first_tick_are_still_loading_not_hang():
    # Live run 2026-10-04: the game answers 503 while it loads, before its loop has ticked once.
    s = [Sample(t, "c1", None, 503) for t in (6, 11, 16)]
    assert outcome.classify(scn(), s, recs(), 17, False).kind == "running"

def test_503s_after_ticking_while_loading_is_hang():
    s = [Sample(2, "c1", cli(has_pawn=False, ticks=5), 200)] + [Sample(t, "c1", None, 503) for t in range(6, 6 + 72, 4)]
    assert outcome.classify(scn(), s, recs(), 80, False).kind == "hang"

def test_never_ticking_game_times_out_in_startup():
    s = [Sample(t, "c1", None, 503) for t in range(0, 250, 5)]
    o = outcome.classify(scn(), s, recs(), outcome.STARTUP_TIMEOUT_S, False)
    assert (o.kind, o.phase) == ("timeout", "startup")

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

def test_desync_looks_up_server_location_by_unique_id():
    uid = "01000000000000000000000000000000"
    server = srv(player_locations={uid: [0, 0, 0]})   # server keys by UniqueId, not instance name
    far = cli(unique_id=uid, pawn_location=[2000, 0, 0])
    s = [x for t in range(6) for x in pair(1 + 2 * t, s=server, c=far)]
    assert outcome.classify(scn(), s, recs(), 60, False).kind == "desync"

def test_desync_unique_id_mismatch_is_not_compared():
    server = srv(player_locations={"01000000000000000000000000000000": [0, 0, 0]})
    far = cli(unique_id="02000000000000000000000000000000", pawn_location=[2000, 0, 0])
    s = [x for t in range(6) for x in pair(1 + 2 * t, s=server, c=far)]
    assert outcome.classify(scn(), s, recs(), 60, False).kind == "running"

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

def test_missing_unique_id_is_noted_on_outcome():
    o = outcome.classify(scn(), pair(1) + pair(3), recs(), 900, False)
    assert o.kind == "pass" and "no unique_id" in o.detail and "c1" in o.detail

def test_present_unique_id_adds_no_note():
    c = cli(unique_id="01000000000000000000000000000000")
    o = outcome.classify(scn(), pair(1, c=c) + pair(3, c=c), recs(), 900, False)
    assert (o.kind, o.detail) == ("pass", "survived")


# --- Fix round 1 -----------------------------------------------------------------------------

def _frozen_c1(ok_until, frozen_until, step=5):
    """Both answer until ok_until (ticks rising); then the server answers and c1 returns 503."""
    s = []
    for i, t in enumerate(range(0, ok_until + 1, step)):
        s += pair(t, s=srv(ticks=10 + i * 100), c=cli(ticks=10 + i * 100))
    for t in range(ok_until + step, frozen_until + 1, step):
        s += [Sample(t, "server", srv(), 200), Sample(t, "c1", None, 503)]
    return s

def test_freeze_near_the_time_limit_is_not_pass():
    # Reviewer reproducer: 120 s survive run, freeze at t=80, 503s from 85 to 120 graded pass.
    o = outcome.classify(scn(limit=120), _frozen_c1(80, 120), recs(), 120, False)
    assert o.kind == "running"

def test_freeze_near_the_time_limit_ends_as_hang():
    o = outcome.classify(scn(limit=120), _frozen_c1(80, 150), recs(), 150, False)
    assert (o.kind, o.process) == ("hang", "c1")

def test_freeze_near_the_time_limit_with_watchdog_report_is_hang_with_frames():
    r = recs(c1=ProcessRecord("c1", "client", None, [], {"seconds": 60, "frames": ["reborn+0x10"]}, []))
    o = outcome.classify(scn(limit=120), _frozen_c1(80, 125), r, 125, False)
    assert (o.kind, o.frame) == ("hang", "reborn+0x10")

def test_overtime_is_capped_at_watchdog_limit_plus_grace():
    # Even if the samples have not yet shown a long enough freeze, the run may not go on forever.
    s = _frozen_c1(80, 120) + [Sample(190, "server", srv(), 200)]
    o = outcome.classify(scn(limit=120), s, recs(), 120 + 60 + outcome.WATCHDOG_GRACE_S, False)
    assert (o.kind, o.process) == ("hang", "c1")

def test_brief_503_that_recovers_before_the_limit_still_passes():
    s = _frozen_c1(80, 90) + pair(95) + pair(120)
    assert outcome.classify(scn(limit=120), s, recs(), 120, False).kind == "pass"

def test_match_end_is_not_pass_while_a_client_is_unresponsive():
    r = recs(server=ProcessRecord("server", "server", 0, [], None, []))
    o = outcome.classify(scn("match_end"), _frozen_c1(80, 100), r, 100, True)
    assert o.kind == "running"

def test_stale_game_thread_fault_does_not_turn_a_later_freeze_into_a_crash():
    # Reviewer reproducer: a handled first-chance fault at tick 100, the game keeps ticking,
    # later a real freeze -> must be the hang, with the hang report's frames.
    rep = {"code": "0xC0000005", "frames": ["battleborn+0x10"], "game_thread": True,
           "first_chance": True, "ticks": 100}
    hang = {"seconds": 60, "frames": ["reborn+0xd850a", "battleborn+0xee67af"]}
    r = recs(c1=ProcessRecord("c1", "client", None, [rep], hang, []))
    s = pair(1, c=cli(ticks=50)) + pair(5, c=cli(ticks=400)) + [Sample(t, "c1", None, 503) for t in (9, 13, 17)]
    o = outcome.classify(scn(), s, r, 60, False)
    assert (o.kind, o.frame) == ("hang", "reborn+0xd850a|battleborn+0xee67af")

def test_game_thread_fault_counts_when_ticks_never_passed_it():
    rep = {"code": "0xC0000005", "frames": ["reborn+0xd84d4"], "game_thread": True,
           "first_chance": True, "ticks": 400}
    r = recs(c1=ProcessRecord("c1", "client", None, [rep], None, []))
    s = pair(1, c=cli(ticks=50)) + pair(5, c=cli(ticks=400)) + [Sample(t, "c1", None, 503) for t in (9, 13, 17)]
    assert outcome.classify(scn(), s, r, 60, False).kind == "crash"

def _limit(*args):
    sc = scn()
    sc.processes[1].args = list(args)
    return outcome._watchdog_limit(sc, "c1")

def test_watchdog_limit_follows_the_mods_rules():
    assert _limit("-rbhangsecs=15") == 15
    assert _limit("-rbhangsecs=5") == 5 and _limit("-rbhangsecs=3600") == 3600
    assert _limit("-rbhangsecs=4") == 60 and _limit("-rbhangsecs=3601") == 60
    assert _limit("-rbhangsecs=abc") == 60 and _limit("-rbhangsecs=15x") == 60
    assert _limit("-RBHANGSECS=15") == 60            # the mod matches the flag case-sensitively
    assert _limit("-rbhangsecs=15", "-rbhangsecs=20") == 20   # last one wins
    assert _limit("-rbhangsecs=15", "-rbhangsecs=2") == 15    # a bad value leaves the earlier one


# --- Fix round 2 -----------------------------------------------------------------------------

def scn2(limit=900):
    text = f'''
name="t2"
step=2
time_limit_s={limit}
pass_when="match_end"
expect_map="Dojo_P"
[[process]]
name="server"
role="server"
args=[]
[[process]]
name="c1"
role="client"
args=[]
[[process]]
name="c2"
role="client"
args=[]
'''
    return scenario.parse(text, Path("t2.toml"))

def _two_clients_playing(until):
    s = []
    for t in range(0, until + 1, 5):
        s += [Sample(t, "server", srv(connections=2, player_locations={}), 200),
              Sample(t, "c1", cli(ticks=10 + t), 200), Sample(t, "c2", cli(ticks=10 + t), 200)]
    return s

def _after_match(ts, c1_status=503):
    """Server has exited (no answer), c1 silent, c2 back in the menu and disconnected."""
    s = []
    for t in ts:
        s += [Sample(t, "server", None, None, alive=False),
              Sample(t, "c1", None, c1_status, alive=True),
              Sample(t, "c2", cli(connected=False, disconnect_reason="lost", map="MenuMap_P"), 200, alive=True)]
    return s

def _recs2():
    return [ProcessRecord("server", "server", 0, [], None, []),
            ProcessRecord("c1", "client", None, [], None, []),
            ProcessRecord("c2", "client", None, [], None, [])]

def test_match_end_on_hold_ignores_post_match_disconnect():
    # Reviewer reproducer: server exited 0 with match ended, c1 answers 503, c2 reports
    # connected=False -> was graded "disconnect client:lost".
    s = _two_clients_playing(50) + _after_match([55, 60])
    assert outcome.classify(scn2(), s, _recs2(), 60, True).kind == "running"

def test_match_end_on_hold_passes_once_everyone_answers():
    s = _two_clients_playing(50) + _after_match([55, 60]) + [Sample(65, "c1", cli(map="MenuMap_P"), 200, alive=True)]
    o = outcome.classify(scn2(), s, _recs2(), 65, True)
    assert o.kind == "pass" and o.detail.startswith("match_end")

def test_match_end_on_hold_ends_as_hang():
    s = _two_clients_playing(50) + _after_match(range(55, 55 + 75, 5))
    o = outcome.classify(scn2(), s, _recs2(), 130, True)
    assert (o.kind, o.process) == ("hang", "c1")

def test_match_end_on_hold_hang_at_the_cap():
    s = _two_clients_playing(50) + _after_match([55, 60])
    o = outcome.classify(scn2(limit=100), s, _recs2(), 100 + 60 + outcome.WATCHDOG_GRACE_S, True)
    assert (o.kind, o.process) == ("hang", "c1")

def _timing_out_c1(ok_until, until, step=5):
    s = []
    for i, t in enumerate(range(0, ok_until + 1, step)):
        s += pair(t, c=cli(ticks=10 + i * 100))
    for t in range(ok_until + step, until + 1, step):
        s += [Sample(t, "server", srv(), 200, alive=True), Sample(t, "c1", None, None, alive=True)]
    return s

def test_alive_process_whose_polls_time_out_is_not_pass():
    o = outcome.classify(scn(limit=120), _timing_out_c1(80, 120), recs(), 120, False)
    assert o.kind == "running"

def test_alive_process_whose_polls_time_out_ends_as_hang():
    o = outcome.classify(scn(limit=120), _timing_out_c1(80, 120), recs(), 120 + 60 + outcome.WATCHDOG_GRACE_S, False)
    assert (o.kind, o.process) == ("hang", "c1")
    o = outcome.classify(scn(limit=120), _timing_out_c1(80, 150), recs(), 150, False)
    assert (o.kind, o.process) == ("hang", "c1")

def test_exited_process_with_no_answer_does_not_hold_a_match_end_pass():
    s = pair(1) + [Sample(t, "server", None, None, alive=False) for t in (3, 5, 7)] + [Sample(7, "c1", cli(), 200)]
    r = recs(server=ProcessRecord("server", "server", 0, [], None, []))
    assert outcome.classify(scn("match_end"), s, r, 8, True).kind == "pass"


# --- milestone: how far through the join a run got -------------------------------------------

def test_milestone_empty_is_zero():
    assert outcome.milestone([]) == 0


def test_milestone_follows_the_furthest_autopilot_phase():
    order = ["off", "waiting_for_menu", "menu_ready", "launching", "character_select", "playing"]
    got = [outcome.milestone([Sample(1, "c1", {"ticks": 1, "autopilot": ph}, 200)]) for ph in order]
    assert got == [0, 0, 1, 2, 3, 4]


def test_milestone_never_goes_back_when_a_client_regresses():
    s = [Sample(1, "c1", {"autopilot": "character_select"}, 200),
         Sample(2, "c1", {"autopilot": "waiting_for_menu"}, 200)]
    assert outcome.milestone(s) == 3


def test_milestone_takes_the_furthest_of_several_clients():
    s = [Sample(1, "c1", {"autopilot": "launching"}, 200), Sample(1, "c2", {"autopilot": "playing"}, 200)]
    assert outcome.milestone(s) == 4


def test_milestone_unknown_missing_or_bad_phases_count_zero():
    s = [Sample(1, "c1", {"autopilot": "bogus"}, 200), Sample(2, "c1", {"ticks": 3}, 200),
         Sample(3, "c1", None, 503), Sample(4, "c1", {"autopilot": None}, 200),
         Sample(5, "c1", {"autopilot": 7}, 200), Sample(6, "c1", {"autopilot": ["playing"]}, 200)]
    assert outcome.milestone(s) == 0


def test_milestone_connection_adds_one_point():
    s = [Sample(1, "server", srv(connections=1, player_locations={}), 200),
         Sample(1, "c1", {"autopilot": "character_select"}, 200)]
    assert outcome.milestone(s) == 4


def test_milestone_server_player_location_counts_connection_and_spawn():
    s = [Sample(1, "server", srv(connections=0, player_locations={"u1": [0, 0, 0]}), 200)]
    assert outcome.milestone(s) == 2


def test_milestone_server_spawned_pawn_beats_connection_alone():
    # The live case: the client sits in character select; the server has spawned its pawn.
    base = Sample(1, "c1", {"autopilot": "character_select"}, 200)
    no_pawn = [base, Sample(1, "server", srv(connections=1, player_locations={}), 200)]
    pawn = [base, Sample(2, "server", srv(connections=1, player_locations={"0100": [1, 2, 3]}), 200)]
    assert outcome.milestone(no_pawn) == 4 and outcome.milestone(pawn) == 5


def test_milestone_spawn_point_only_from_the_server():
    s = [Sample(1, "c1", {"autopilot": "playing", "player_locations": {"u1": [0, 0, 0]}}, 200)]
    assert outcome.milestone(s) == 5   # playing + connection; a client's locations are no spawn proof
    s = [Sample(1, "srv2", {"role": "server", "player_locations": {"u1": [0, 0, 0]}}, 200)]
    assert outcome.milestone(s) == 2


def test_milestone_connection_counts_once_and_only_when_seen():
    s = [Sample(1, "server", srv(connections=0, player_locations={}), 200),
         Sample(2, "server", srv(connections=2, player_locations={}), 200),
         Sample(3, "server", srv(connections=3, player_locations={}), 200)]
    assert outcome.milestone(s) == 1
    assert outcome.milestone([Sample(1, "server", srv(connections=0, player_locations={}), 200)]) == 0
    assert outcome.milestone([Sample(1, "server", {"ticks": 1, "listening": True}, 200)]) == 0


def test_milestone_ignores_non_numeric_connections():
    s = [Sample(1, "server", srv(connections="many", player_locations=None), 200)]
    assert outcome.milestone(s) == 0


def test_milestone_solo_uses_the_solo_autopilot_phase():
    s = [Sample(1, "game", {"ticks": 5, "autopilot": "playing"}, 200)]
    assert outcome.milestone(s) == 4


# A match the game ends by its own rules (e.g. lost to minions) is a normal end in a survive run.
def _match_lost_at(t_end):
    s = []
    for t in range(0, t_end, 2):
        s += pair(t)
    s += [Sample(t_end, "server", srv(connections=0, player_locations={}, match_over=True), 200),
          Sample(t_end, "c1", cli(connected=False, disconnect_reason="lost", map="MenuMap_P"), 200)]
    return s

def test_survive_run_waits_after_the_game_ends_the_match():
    o = outcome.classify(scn(), _match_lost_at(470), recs(), 471, True)
    assert o.kind == "running"

def test_survive_run_passes_when_the_game_ends_the_match():
    r = recs(server=ProcessRecord("server", "server", 0, [], None, []))
    o = outcome.classify(scn(), _match_lost_at(470), r, 482, True)
    assert o.kind == "pass" and o.detail.startswith("match_end")

def test_disconnect_before_the_match_ends_still_fails():
    s = pair(0) + pair(2, c=cli(connected=False, disconnect_reason="lost")) + pair(4)
    s += [Sample(6, "server", srv(connections=0, match_over=True), 200)]
    o = outcome.classify(scn(), s, recs(), 7, True)
    assert o.kind == "disconnect" and o.detail == "client:lost"


def test_client_reporting_match_over_stops_desync_checks():
    # Run 20261005-044519: c1 showed DEFEAT and its pawn stopped while the server's copy kept
    # moving for ~10 s before the server logged "Match ended" -> was graded desync.
    s = []
    for t in range(0, 20, 2):
        s += pair(t)
    for t in range(20, 40, 2):
        s += [Sample(t, "server", srv(player_locations={"c1": [5000 + 300 * t, 0, 0]}), 200),
              Sample(t, "c1", cli(match_over=True), 200)]
    assert outcome.classify(scn(), s, recs(), 40, False).kind == "running"


def _falling_client(fall_every_s, until):
    """Pair samples where c1 drops 9000 units below its start every fall_every_s seconds."""
    s = []
    for t in range(0, until, 2):
        low = t > 10 and (t % fall_every_s) < 6
        loc = [0, 0, -12000 if low else -3000]
        s += pair(t, s=srv(player_locations={"c1": loc}), c=cli(pawn_location=loc))
    return s

def test_player_who_keeps_falling_fails_the_run():
    o = outcome.classify(scn(), _falling_client(30, 200), recs(), 200, False)
    assert (o.kind, o.process) == ("fell", "c1")

def test_falling_off_a_ledge_now_and_then_is_normal():
    o = outcome.classify(scn(), _falling_client(400, 900), recs(), 900, False)
    assert o.kind == "pass", o

def test_fell_signature_names_the_map():
    from debugloop import signature
    assert signature.make(outcome.Outcome("fell", "c1", "c1", "playing"), scn()) == "fell:Dojo_P"


def test_dead_player_is_not_a_desync():
    s = []
    for t in range(0, 20, 2):
        s += pair(t)
    for t in range(20, 40, 2):
        s += [Sample(t, "server", srv(player_locations={"c1": [0, 0, -1000 * t]}), 200),
              Sample(t, "c1", cli(pawn_health=0.0, pawn_location=[0, 0, -9000]), 200)]
    assert outcome.classify(scn(), s, recs(), 40, False).kind == "running"

def test_live_player_far_from_server_is_still_a_desync():
    s = []
    for t in range(0, 30, 2):
        s += [Sample(t, "server", srv(player_locations={"c1": [9000, 0, 0]}), 200),
              Sample(t, "c1", cli(pawn_health=500.0), 200)]
    assert outcome.classify(scn(), s, recs(), 30, False).kind == "desync"


def _body_samples(until, c, step=4):
    """Pair samples every `step` s up to `until`; the client's sample gets the fields c(t) returns."""
    return [x for t in range(0, until, step) for x in pair(t, c=cli(**c(t)))]

def test_player_without_a_visible_body_fails_the_run():
    s = _body_samples(200, c=lambda t: dict(pawn_visible=False, pawn_body_missing="no mesh component",
                                            pawn_health=1091.0))
    o = outcome.classify(scn(), s, recs(), 200, False)
    assert (o.kind, o.process) == ("invisible", "c1"), o
    assert "no mesh component" in o.detail

def test_body_that_loads_during_the_grace_period_is_normal():
    s = _body_samples(900, c=lambda t: dict(pawn_visible=t >= 24, pawn_health=1091.0))
    assert outcome.classify(scn(), s, recs(), 900, False).kind == "pass"

def test_body_missing_for_less_than_the_persist_time_is_normal():
    s = _body_samples(900, c=lambda t: dict(pawn_visible=not (100 <= t < 150), pawn_health=1091.0))
    assert outcome.classify(scn(), s, recs(), 900, False).kind == "pass"

def test_seeing_another_player_without_a_body_fails_the_run():
    s = _body_samples(200, c=lambda t: dict(pawn_visible=True, pawn_health=1091.0, others_seen=1,
                                            others_invisible=["LAN Player (Class_ModernSoldier): no mesh component"]))
    o = outcome.classify(scn(), s, recs(), 200, False)
    assert (o.kind, o.process) == ("invisible", "c1"), o
    assert "Class_ModernSoldier" in o.detail

def test_dead_player_without_a_body_is_not_invisible():
    s = _body_samples(900, c=lambda t: dict(pawn_visible=False, pawn_health=0.0))
    assert outcome.classify(scn(), s, recs(), 900, False).kind == "pass"

def test_timeline_without_body_fields_is_not_judged():
    s = _body_samples(900, c=lambda t: dict(pawn_health=1091.0))
    assert outcome.classify(scn(), s, recs(), 900, False).kind == "pass"

def test_respawn_starts_the_grace_period_again():
    # Invisible while alive 0-20 s, dead 20-60 s, then a new pawn with no body until 140 s: counted
    # from the respawn that is 30 s of grace + 48 s, short of the persist time.
    def c(t):
        if 20 <= t < 60:
            return dict(pawn_visible=False, pawn_health=0.0)
        return dict(pawn_visible=t >= 140, pawn_health=1091.0)
    s = _body_samples(900, c=c)
    assert outcome.classify(scn(), s, recs(), 900, False).kind == "pass"

def test_invisible_body_after_the_match_ended_is_not_judged():
    s = []
    for t in range(0, 300, 4):
        s += pair(t, s=srv(match_over=t >= 20), c=cli(pawn_visible=False, pawn_health=1091.0))
    assert outcome.classify(scn(), s, recs(), 900, False).kind == "pass"

def test_invisible_signature_names_the_map():
    from debugloop import signature
    o = outcome.Outcome("invisible", "c2: own pawn has no visible body", "c2", "playing")
    assert signature.make(o, scn()) == "invisible:Dojo_P"

"""A combat-brain player that never fights, on a map with enemies (Scenario.expect_combat)."""
import dataclasses
import json
from pathlib import Path

from debugloop import loop, outcome, scenario, signature
from debugloop.outcome import ProcessRecord, Sample
from debugloop.tests.test_loop import deps
from debugloop.tests.test_outcome import cli, recs, scn, srv
from debugloop.tests.test_scenario import GOOD


def fighting_scn(expect=True):
    return dataclasses.replace(scn(), expect_combat=expect)


def run_of(stats, enabled=True, until=300, step=30, pawn_from=0):
    """Server and c1 samples every `step` s up to `until`; c1 has a pawn from `pawn_from` on."""
    out = []
    for t in range(0, until + 1, step):
        c = cli(has_pawn=t >= pawn_from, combat={"enabled": enabled, "stats": stats})
        out += [Sample(t, "server", srv(), 200), Sample(t, "c1", c, 200)]
    return out


IDLE = {"shots": 0, "skills_used": 0, "kills": 0, "damage_taken": 0}


# Scenario field

def test_expect_combat_defaults_to_false():
    assert scenario.parse(GOOD, Path("x.toml")).expect_combat is False


def test_expect_combat_true_is_read():
    text = GOOD.replace('expect_map = "Dojo_P"', 'expect_map = "Dojo_P"\nexpect_combat = true')
    assert scenario.parse(text, Path("x.toml")).expect_combat is True


def test_expect_combat_must_be_a_bool():
    text = GOOD.replace('expect_map = "Dojo_P"', 'expect_map = "Dojo_P"\nexpect_combat = "yes"')
    try:
        scenario.parse(text, Path("x.toml"))
    except scenario.ScenarioError as e:
        assert "expect_combat" in str(e)
    else:
        raise AssertionError("a non-bool expect_combat was accepted")


def test_real_scenarios_expect_combat_only_where_enemies_are():
    want = {"s1-dojo-1client", "s1-dojo-1client-smoke",
            "s2-algorithm-2clients", "s2-algorithm-2clients-smoke"}
    got = {s.name for s in scenario.load_all() if s.expect_combat}
    assert got == want


# Classification

def test_pass_with_an_idle_fighter_becomes_nocombat():
    o = outcome.classify(fighting_scn(), run_of(IDLE), recs(), 900, False)
    assert (o.kind, o.process, o.phase) == ("nocombat", "c1", "playing"), o
    assert o.detail == ("c1 had the combat brain on for 300 s but never fired or used a skill; "
                        "on this map it should find and fight enemies")
    assert signature.make(o, fighting_scn()) == "nocombat:Dojo_P"


def test_match_end_pass_with_an_idle_fighter_becomes_nocombat():
    r = recs(server=ProcessRecord("server", "server", 0, [], None, []))
    o = outcome.classify(fighting_scn(), run_of(IDLE), r, 400, True)
    assert o.kind == "nocombat", o


def test_playing_time_counts_from_the_pawn():
    # In the game from 150 s to 300 s: 150 s of play is not enough to judge.
    o = outcome.classify(fighting_scn(), run_of(IDLE, pawn_from=150), recs(), 900, False)
    assert o.kind == "pass", o


def test_autopilot_playing_counts_as_in_the_game():
    samples = run_of(IDLE, pawn_from=240)   # pawn only late, autopilot "playing" from the start
    for s in samples:
        if s.name == "c1":
            s.state["autopilot"] = "playing"
    o = outcome.classify(fighting_scn(), samples, recs(), 900, False)
    assert o.kind == "nocombat", o


def test_one_shot_or_one_skill_is_enough():
    for stats in ({**IDLE, "shots": 1}, {**IDLE, "skills_used": 1}):
        o = outcome.classify(fighting_scn(), run_of(stats), recs(), 900, False)
        assert o.kind == "pass", (stats, o)


def test_no_nocombat_without_expect_combat():
    o = outcome.classify(fighting_scn(False), run_of(IDLE), recs(), 900, False)
    assert o.kind == "pass", o


def test_no_nocombat_when_the_brain_is_off():
    o = outcome.classify(fighting_scn(), run_of(IDLE, enabled=False), recs(), 900, False)
    assert o.kind == "pass", o


def test_no_nocombat_without_a_combat_block():
    samples = run_of(IDLE)
    for s in samples:
        s.state.pop("combat", None)
    assert outcome.classify(fighting_scn(), samples, recs(), 900, False).kind == "pass"


def test_no_nocombat_under_180_s_of_play():
    o = outcome.classify(fighting_scn(), run_of(IDLE, until=150), recs(), 900, False)
    assert o.kind == "pass", o


def test_a_failed_run_keeps_its_own_outcome():
    rep = {"code": "0xC0000005", "frames": ["battleborn+0x10"], "first_chance": True}
    r = recs(c1=ProcessRecord("c1", "client", 3, [rep], None, []))
    assert outcome.classify(fighting_scn(), run_of(IDLE), r, 900, False).kind == "crash"
    # Still running (before the time limit): not judged yet.
    assert outcome.classify(fighting_scn(), run_of(IDLE), recs(), 400, False).kind == "running"
    # Startup never finished: a timeout, not nocombat.
    stuck = [Sample(t, "c1", cli(has_pawn=False, ticks=10, combat={"enabled": True, "stats": IDLE}), 200)
             for t in range(0, 300, 30)]
    assert outcome.classify(fighting_scn(), stuck, recs(), 900, False).kind == "timeout"


# The loop opens it like any other bug

def test_loop_opens_nocombat_bug_with_a_plain_brief(tmp_path):
    detail = ("c1 had the combat brain on for 240 s but never fired or used a skill; "
              "on this map it should find and fight enemies")
    d = deps(tmp_path, [("nocombat", "nocombat:Dojo_P", "playing", 480.0)])
    inner = d.run

    def run_with_result(scn):
        r = inner(scn)
        (r.run_dir / "result.json").write_text(json.dumps(
            {"outcome": {"kind": "nocombat", "detail": detail, "process": "c1", "phase": "playing"}}))
        return r

    d.run = run_with_result
    assert loop.cmd_next(d) == loop.FIX_NEEDED
    brief = (tmp_path / "state" / "brief.md").read_text()
    assert "`nocombat:Dojo_P`" in brief
    assert "## What went wrong" in brief and detail + "." in brief
    assert d.triaged


def test_brief_of_other_bugs_has_no_nocombat_section(tmp_path):
    d = deps(tmp_path, [("exit", "exit:3", "playing", 100)])
    assert loop.cmd_next(d) == loop.FIX_NEEDED
    assert "## What went wrong" not in (tmp_path / "state" / "brief.md").read_text()

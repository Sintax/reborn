"""The picked hero loads (wronghero) in its own skin (wrongskin)."""
import json

from debugloop import loop, outcome, signature
from debugloop.tests.test_loop import deps
from debugloop.tests.test_outcome import cli, pair, recs, scn

ALIVE = {"pawn_health": 1091.0, "pawn_visible": True}


def _samples(until, c, step=4):
    """Pair samples every `step` s up to `until`; the client's sample gets the fields c(t) returns."""
    return [x for t in range(0, until, step) for x in pair(t, c=cli(**{**ALIVE, **c(t)}))]


def judge(samples, elapsed=900):
    return outcome.classify(scn(), samples, recs(), elapsed, False)


WRONG = dict(requested_hero="Marquis", pawn_hero="Alani", hero_matches=False)
OWN_SKIN = dict(pawn_hero="Oscar Mike", pawn_skin_ok=False,
                pawn_skin_wrong="Benedict's skin Skin_Default on Class_ModernSoldier")


# wronghero

def test_wrong_hero_fails_the_run():
    o = judge(_samples(200, lambda t: WRONG), 200)
    assert (o.kind, o.process) == ("wronghero", "c1"), o
    assert o.detail == "c1: picked Marquis but plays Alani"
    assert signature.make(o, scn()) == "wronghero:Dojo_P"


def test_hero_that_settles_during_the_grace_period_is_normal():
    o = judge(_samples(900, lambda t: WRONG if t < 24 else dict(WRONG, hero_matches=True)))
    assert o.kind == "pass", o


def test_wrong_hero_for_less_than_the_persist_time_is_normal():
    o = judge(_samples(900, lambda t: dict(WRONG, hero_matches=not (100 <= t < 150))))
    assert o.kind == "pass", o


def test_unknown_or_missing_hero_match_is_not_judged():
    for fields in ({"hero_matches": None}, {}, {"requested_hero": None, "pawn_hero": "Alani"}):
        assert judge(_samples(900, lambda t: fields)).kind == "pass", fields


def test_dead_player_of_the_wrong_hero_is_skipped():
    o = judge(_samples(900, lambda t: dict(WRONG, pawn_health=0.0)))
    assert o.kind == "pass", o


def test_wrong_hero_after_the_match_ended_is_not_judged():
    s = []
    for t in range(0, 300, 4):
        s += pair(t, s={**pair(0)[0].state, "match_over": t >= 20}, c=cli(**ALIVE, **WRONG))
    assert judge(s).kind == "pass"


# wrongskin

def test_own_pawn_in_another_heros_skin_fails_the_run():
    o = judge(_samples(200, lambda t: OWN_SKIN), 200)
    assert (o.kind, o.process) == ("wrongskin", "c1"), o
    assert "Benedict's skin" in o.detail and "Oscar Mike" in o.detail
    assert signature.make(o, scn()) == "wrongskin:Dojo_P"


def test_seeing_another_player_in_the_wrong_skin_fails_the_run():
    o = judge(_samples(200, lambda t: dict(pawn_skin_ok=True, others_wrong_skin=[
        "LAN Player (Class_ModernSoldier): Benedict's skin Skin_Default on Class_ModernSoldier"])), 200)
    assert (o.kind, o.process) == ("wrongskin", "c1"), o
    assert "LAN Player" in o.detail


def test_skin_fields_ok_or_missing_are_normal():
    for fields in ({"pawn_skin_ok": True, "others_wrong_skin": []}, {"pawn_skin_ok": None}, {}):
        assert judge(_samples(900, lambda t: fields)).kind == "pass", fields


def test_wrong_skin_during_the_grace_period_is_normal():
    o = judge(_samples(900, lambda t: OWN_SKIN if t < 24 else {"pawn_skin_ok": True}))
    assert o.kind == "pass", o


def test_dead_player_in_the_wrong_skin_is_skipped():
    o = judge(_samples(900, lambda t: dict(OWN_SKIN, pawn_health=0.0)))
    assert o.kind == "pass", o


def test_wrong_skin_is_named_before_the_missing_body_it_causes():
    o = judge(_samples(200, lambda t: dict(OWN_SKIN, pawn_visible=False)), 200)
    assert o.kind == "wrongskin", o


def test_wrong_hero_is_named_before_the_wrong_skin():
    o = judge(_samples(200, lambda t: dict(OWN_SKIN, **WRONG)), 200)
    assert o.kind == "wronghero", o


# The loop's brief says it in plain words

def test_loop_brief_names_the_wrong_hero(tmp_path):
    detail = "c2: picked Marquis but plays Alani"
    d = deps(tmp_path, [("wronghero", "wronghero:Caverns_P", "playing", 200.0)])
    inner = d.run

    def run_with_result(scn):
        r = inner(scn)
        (r.run_dir / "result.json").write_text(json.dumps(
            {"outcome": {"kind": "wronghero", "detail": detail, "process": "c2", "phase": "playing"}}))
        return r

    d.run = run_with_result
    assert loop.cmd_next(d) == loop.FIX_NEEDED
    brief = (tmp_path / "state" / "brief.md").read_text()
    assert "## What went wrong" in brief and detail + "." in brief
    assert "timeline.jsonl" in brief and "combat.json" not in brief

"""Server-vs-client comparison of computer-controlled characters (debugloop/npcsync.py)."""
import json

from debugloop import npcsync, run, scenario, signature
from debugloop.outcome import Outcome, Sample


def npc(a, x, y=0, hp=100, k="pawn", **kw):
    return {"k": k, "a": a, "c": "C_" + a, "p": [x, y, 0], "hp": hp, **kw}


def rounds(server_npcs_by_t, client_npcs_by_t, me=(0, 0, 0), client="c1"):
    out = []
    for t in sorted(server_npcs_by_t):
        out.append(Sample(t, "server", {"npcs": server_npcs_by_t[t]}, 200, True))
        out.append(Sample(t + 0.1, client, {"npcs": client_npcs_by_t.get(t, []), "has_pawn": True,
                                            "pawn_location": list(me)}, 200, True))
    return out


# One round

def test_matching_copies_are_fine_even_a_little_apart():
    f = npcsync.compare_round([npc("Thrall", 500)], [npc("Thrall", 900)], [0, 0, 0])
    assert f == {"missing": [], "ghost": [], "invisible": []}


def test_far_away_server_characters_are_not_expected_on_the_client():
    f = npcsync.compare_round([npc("Thrall", 20000)], [], [0, 0, 0])
    assert f["missing"] == []


def test_missing_ghost_and_invisible():
    f = npcsync.compare_round([npc("Thrall", 500), npc("Aurox", 300)],
                              [npc("Aurox", 300, body="no mesh component"), npc("Varelsi", 200)], [0, 0, 0])
    assert [e["a"] for e in f["missing"]] == ["Thrall"]
    assert [e["a"] for e in f["ghost"]] == ["Varelsi"]
    assert [e["a"] for e in f["invisible"]] == ["Aurox"]


def test_another_archetype_close_by_is_no_match():
    f = npcsync.compare_round([npc("Thrall", 500)], [npc("Varelsi", 500)], [0, 0, 0])
    assert len(f["missing"]) == 1 and len(f["ghost"]) == 1


def test_dead_characters_are_ignored():
    f = npcsync.compare_round([npc("Thrall", 500, hp=0)], [npc("Varelsi", 100, hp=0)], [0, 0, 0])
    assert f == {"missing": [], "ghost": [], "invisible": []}


# Over time

def test_a_short_blip_is_not_a_failure():
    srv = {t: [npc("Thrall", 500)] for t in range(0, 40, 2)}
    cli = {t: [npc("Thrall", 500)] for t in range(0, 40, 2) if t not in (10, 12, 14)}
    rep = npcsync.analyse(rounds(srv, cli))
    assert rep.failures() == [] and len(rep.episodes) == 1 and rep.episodes[0].seconds == 4


def test_a_lasting_disagreement_fails_and_follows_a_moving_character():
    srv = {t: [npc("Thrall", 200 + t * 30)] for t in range(0, 40, 2)}   # walks 60 units a round
    rep = npcsync.analyse(rounds(srv, {}))
    (f,) = rep.failures()
    assert (f.kind, f.archetype, f.seconds) == ("missing", "Thrall", 38)
    assert rep.rounds == {"c1": 20} and rep.archetypes == {"Thrall": 1}


def test_client_samples_without_a_pawn_or_far_in_time_are_skipped():
    s = [Sample(0, "server", {"npcs": [npc("Thrall", 500)]}, 200, True),
         Sample(0.5, "c1", {"npcs": [], "has_pawn": False, "pawn_location": None}, 200, True),
         Sample(10, "c1", {"npcs": [], "has_pawn": True, "pawn_location": [0, 0, 0]}, 200, True)]
    assert npcsync.analyse(s).rounds == {}


# In a run

def _scn(tmp_path, check=True):
    text = f"""name = "x"
step = 2
time_limit_s = 60
check_npcs = {str(check).lower()}
cast_skills_every_s = 6
[[process]]
name = "server"
role = "server"
[[process]]
name = "c1"
role = "client"
"""
    return scenario.parse(text, tmp_path / "x.toml")


def test_scenario_fields(tmp_path):
    s = _scn(tmp_path)
    assert s.check_npcs and s.cast_skills_every_s == 6
    import pytest
    with pytest.raises(scenario.ScenarioError):
        scenario.parse("name='x'\nstep=2\ntime_limit_s=6\ncast_skills_every_s=-1\n"
                       "[[process]]\nname='s'\nrole='solo'\n", tmp_path / "y.toml")


def test_a_passing_run_fails_on_a_lasting_disagreement_and_says_which(tmp_path):
    srv = {t: [npc("Thrall", 500)] for t in range(0, 40, 2)}
    o = run.check_npcs(_scn(tmp_path), rounds(srv, {}), Outcome("pass", "survived", phase="playing"), tmp_path)
    assert o.kind == "npcsync" and o.code == "missing:Thrall" and o.process == "c1"
    assert signature.make(o, _scn(tmp_path)) == "npcsync:missing:Thrall"
    assert json.loads((tmp_path / "npcsync.json").read_text())["failures"]


def test_without_check_npcs_the_report_is_written_but_the_run_still_passes(tmp_path):
    srv = {t: [npc("Thrall", 500)] for t in range(0, 40, 2)}
    o = run.check_npcs(_scn(tmp_path, check=False), rounds(srv, {}), Outcome("pass"), tmp_path)
    assert o.kind == "pass" and (tmp_path / "npcsync.json").exists()


def test_other_failures_are_left_alone(tmp_path):
    srv = {t: [npc("Thrall", 500)] for t in range(0, 40, 2)}
    o = run.check_npcs(_scn(tmp_path), rounds(srv, {}), Outcome("crash", code="C0000005"), tmp_path)
    assert o.kind == "crash"


def test_server_only_things_are_counted_but_not_required_on_clients():
    bomb = npc("Bomb", 300, k="thing", c="PoplarServerSideProjectile")
    f = npcsync.compare_round([bomb], [], [0, 0, 0])
    assert f == {"missing": [], "ghost": [], "invisible": []}
    rep = npcsync.analyse(rounds({t: [bomb] for t in range(0, 30, 2)}, {}))
    assert rep.failures() == [] and rep.archetypes == {"Bomb": 1}

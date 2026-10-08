"""The hero matrix sweep (debugloop/matrix.py), with fake runs: no real game."""
import json

import pytest

from debugloop import matrix, run, scenario
from debugloop.outcome import Outcome
from debugloop.run import RunResult
from debugloop.tests.test_loop import deps

HEROES = ["Alani", "Ambra", "Attikus", "Beatrix", "Benedict", "Boldur"]


# Pairing

def _covered(pairs):
    return {h for p in pairs for h in p}


@pytest.mark.parametrize("heroes", [HEROES, HEROES[:5], HEROES[:2], HEROES[:3]])
def test_every_hero_plays_in_every_round(heroes):
    for rnd in range(3):
        pairs = matrix.pairs_for_round(heroes, rnd)
        assert _covered(pairs) == set(heroes), (rnd, pairs)
        assert all(a != b for a, b in pairs), pairs


def test_round_two_has_new_partners_and_swapped_roles():
    r1, r2 = matrix.pairs_for_round(HEROES, 0), matrix.pairs_for_round(HEROES, 1)
    assert r1 == [("Alani", "Ambra"), ("Attikus", "Beatrix"), ("Benedict", "Boldur")]
    partners1 = {frozenset(p) for p in r1}
    assert not partners1 & {frozenset(p) for p in r2}, r2
    c1_before = {a for a, _ in r1}
    assert all(b in c1_before for _, b in r2), "round 1's c1 heroes are c2 in round 2"
    assert all(a not in c1_before for a, _ in r2)


def test_all_real_heroes_are_read_from_constants():
    heroes = matrix.all_heroes()
    assert len(heroes) == 30 and "Oscar Mike" in heroes and "Shayne & Aurox" in heroes
    assert len(matrix.plan(heroes, 2)) == 30   # 15 pairs a round, none repeated


def test_plan_names_are_sanitised_and_unique():
    items = matrix.plan(["Shayne & Aurox", "Oscar Mike", "Kid Ultra"], 2)
    assert items[0]["name"] == "matrix-shayne-aurox-oscar-mike"
    assert len({i["name"] for i in items}) == len(items)


def test_parse_heroes_matches_names_and_refuses_unknown_ones():
    assert matrix.parse_heroes("oscar mike, Marquis", ["Marquis", "Oscar Mike"]) == ["Oscar Mike", "Marquis"]
    with pytest.raises(ValueError):
        matrix.parse_heroes("Oscar Mike,Nobody", ["Marquis", "Oscar Mike"])
    with pytest.raises(ValueError):
        matrix.parse_heroes("Marquis", ["Marquis", "Oscar Mike"])


def test_pair_scenario_swaps_in_both_heroes():
    base = scenario.find_scenario(matrix.BASE_SCENARIO)
    text = matrix.scenario_text(base, "matrix-x", ["Shayne & Aurox", "Kid Ultra"])
    s = scenario.parse(text, base.path)
    assert (s.name, s.expect_map, s.expect_combat, s.time_limit_s) == \
           ("matrix-x", base.expect_map, base.expect_combat, base.time_limit_s)
    args = {p.name: p.args for p in s.processes}
    assert "-rbcharacter=Shayne & Aurox" in args["c1"] and "-rbcharacter=Kid Ultra" in args["c2"]
    assert sum(a.startswith("-rbcharacter=") for a in args["c1"] + args["c2"]) == 2
    assert args["server"] == base.processes[0].args
    assert "-rbseed=1" in args["c1"] and "-rbautopilot" in args["c2"]


# The sweep

def matrix_deps(tmp_path, results):
    """Loop Deps whose run writes a timeline and combat.json, and ends as results[scenario] says
    ("pass", a failure kind, or an Exception to raise). Records the runs in d.ran."""
    d = deps(tmp_path, [])
    n = iter(range(1000))

    def fake_run(scn):
        d.log.append("run")
        d.ran.append(scn.name)
        res = results.get(scn.name, "pass")
        if isinstance(res, Exception):
            raise res
        heroes = {p.name: next(a.split("=", 1)[1] for a in p.args if a.startswith("-rbcharacter="))
                  for p in scn.processes if p.role == "client"}
        rid = f"r{next(n)}"
        rd = tmp_path / "runs" / rid
        rd.mkdir(parents=True, exist_ok=True)
        with open(rd / "timeline.jsonl", "w") as tl:
            for name, hero in heroes.items():
                tl.write(json.dumps({"t": 1, "name": name, "state": {"pawn_hero": hero}}) + "\n")
        (rd / "combat.json").write_text(json.dumps({"c1": {"kills": 3, "shots": 40},
                                                    "c2": {"kills": 1, "shots": 9}}))
        sig = None if res == "pass" else f"{res}:Caverns_P"
        return RunResult(rid, scn.name, Outcome(res, "why", phase="playing"), sig, rd, 270.0)

    d.run = fake_run
    return d


def test_sweep_runs_every_pair_and_writes_both_files(tmp_path, capsys):
    d = matrix_deps(tmp_path, {})
    assert matrix.sweep(d, HEROES[:4], 2, False) == matrix.OK
    assert d.log[:3] == ["preconditions", "build", "deploy"] and d.log.count("build") == 1
    assert d.ran == [i["name"] for i in matrix.plan(HEROES[:4], 2)]
    data = json.loads((tmp_path / "state" / "matrix.json").read_text())
    row = data["rows"][0]
    assert (row["c1"], row["c2"], row["round"], row["outcome"]) == ("Alani", "Ambra", 1, "pass")
    assert row["loaded"] == {"c1": "Alani", "c2": "Ambra"}
    assert row["combat"] == {"c1": {"kills": 3, "shots": 40}, "c2": {"kills": 1, "shots": 9}}
    assert row["run_dir"] and row["signature"] is None
    md = (tmp_path / "state" / "matrix.md").read_text()
    assert "| 1 | Alani | Ambra | pass |  | Alani / Ambra | 3/40 | 1/9 | r0 |" in md
    assert "MATRIX DONE: 4 pass, 0 fail" in capsys.readouterr().out


def test_sweep_never_touches_the_ladder_state(tmp_path):
    d = matrix_deps(tmp_path, {})
    matrix.sweep(d, HEROES[:2], 1, False)
    assert sorted(p.name for p in (tmp_path / "state").iterdir()) == ["matrix.json", "matrix.md"]


def test_failures_do_not_stop_the_sweep(tmp_path, capsys):
    d = matrix_deps(tmp_path, {"matrix-alani-ambra": "wronghero"})
    assert matrix.sweep(d, HEROES[:4], 1, False) == matrix.FAILED
    assert len(d.ran) == 2
    rows = json.loads((tmp_path / "state" / "matrix.json").read_text())["rows"]
    assert [(r["outcome"], r["signature"]) for r in rows] == [("wronghero", "wronghero:Caverns_P"),
                                                              ("pass", None)]
    assert "MATRIX DONE: 1 pass, 1 fail" in capsys.readouterr().out


def test_resume_skips_pairs_already_recorded(tmp_path):
    d = matrix_deps(tmp_path, {"matrix-attikus-beatrix": run.HarnessError("port busy")})
    # One error is not three in a row: the sweep goes on, but a pair that never ran is no pass.
    assert matrix.sweep(d, HEROES[:4], 1, False) == matrix.HARNESS
    d2 = matrix_deps(tmp_path, {})
    assert matrix.sweep(d2, HEROES[:4], 1, True) == matrix.OK
    assert d2.ran == ["matrix-attikus-beatrix"]
    rows = json.loads((tmp_path / "state" / "matrix.json").read_text())["rows"]
    assert [r["name"] for r in rows] == ["matrix-alani-ambra", "matrix-attikus-beatrix"]


def test_resume_with_nothing_left_builds_nothing(tmp_path):
    matrix.sweep(matrix_deps(tmp_path, {}), HEROES[:2], 1, False)
    d = matrix_deps(tmp_path, {})
    assert matrix.sweep(d, HEROES[:2], 1, True) == matrix.OK
    assert d.ran == [] and "build" not in d.log


def test_a_fresh_sweep_keeps_the_old_results_aside(tmp_path):
    matrix.sweep(matrix_deps(tmp_path, {}), HEROES[:2], 1, False)
    d = matrix_deps(tmp_path, {})
    matrix.sweep(d, HEROES[:2], 1, False)
    assert d.ran == ["matrix-alani-ambra"]
    assert len(list((tmp_path / "state").glob("matrix-*.json"))) == 1


def test_three_harness_errors_in_a_row_stop_the_sweep(tmp_path, capsys):
    boom = {i["name"]: run.HarnessError("no game") for i in matrix.plan(HEROES, 2)}
    d = matrix_deps(tmp_path, boom)
    assert matrix.sweep(d, HEROES, 2, False) == matrix.HARNESS
    assert len(d.ran) == 3
    assert "MATRIX STOPPED" in capsys.readouterr().out
    assert json.loads((tmp_path / "state" / "matrix.json").read_text())["rows"] == []


def test_a_play_session_or_running_game_is_refused(tmp_path):
    d = matrix_deps(tmp_path, {})
    d.play_alive = lambda: {"scenario": "x", "ports": {}}
    assert matrix.sweep(d, HEROES[:2], 1, False) == matrix.PLAY_SESSION
    d = matrix_deps(tmp_path, {})

    def busy(runs_dir, n):
        raise run.HarnessError("a game process is already running")

    d.preconditions = busy
    assert matrix.sweep(d, HEROES[:2], 1, False) == matrix.HARNESS
    assert d.ran == [] and "build" not in d.log


def test_build_failure_runs_nothing(tmp_path):
    d = matrix_deps(tmp_path, {})
    d.build = lambda: type("B", (), {"ok": False, "output": "error C2065"})()
    assert matrix.sweep(d, HEROES[:2], 1, False) == matrix.HARNESS
    assert d.ran == []


def test_network_pair_scenario_goes_through_the_relay():
    from debugloop import netem
    base = scenario.find_scenario(matrix.BASE_SCENARIO)
    text = matrix.scenario_text(base, "matrix-x-net-bad", ["Kid Ultra", "Marquis"], netem.PRESETS["bad"])
    assert scenario.parse(text, base.path).network == netem.PRESETS["bad"]
    assert scenario.parse(matrix.scenario_text(base, "y", ["Kid Ultra", "Marquis"]), base.path).network is None


def test_network_sweep_keeps_its_own_results_and_names_its_runs(tmp_path):
    matrix.sweep(matrix_deps(tmp_path, {}), HEROES[:2], 1, False)
    d = matrix_deps(tmp_path, {})
    assert matrix.sweep(d, HEROES[:2], 1, False, "bad") == matrix.OK
    assert d.ran == ["matrix-alani-ambra-net-bad"]
    assert sorted(p.name for p in (tmp_path / "state").iterdir()) == \
        ["matrix-net-bad.json", "matrix-net-bad.md", "matrix.json", "matrix.md"]
    assert '"bad" internet relay' in (tmp_path / "state" / "matrix-net-bad.md").read_text()
    d2 = matrix_deps(tmp_path, {})
    assert matrix.sweep(d2, HEROES[:2], 1, True, "bad") == matrix.OK and d2.ran == []

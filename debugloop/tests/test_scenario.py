from pathlib import Path
import pytest
from debugloop import scenario

GOOD = """
name = "s1-dojo-1client"
step = 1
time_limit_s = 900
pass_when = "survive"
required_passes = 3
expect_map = "Dojo_P"

[[process]]
name = "server"
role = "server"
args = ["-rbservermap=Dojo_P"]

[[process]]
name = "c1"
role = "client"
args = []
"""


def test_parse_good():
    s = scenario.parse(GOOD, Path("x.toml"))
    assert s.step == 1 and s.smoke is False
    assert [p.role for p in s.processes] == ["server", "client"]
    assert s.expect_map == "Dojo_P"


@pytest.mark.parametrize("bad,msg", [
    (GOOD.replace('pass_when = "survive"', 'pass_when = "maybe"'), "pass_when"),
    (GOOD.replace('role = "client"', 'role = "ghost"'), "role"),
    (GOOD.replace("step = 1", "step = 9"), "step"),
    (GOOD.replace('name = "c1"', 'name = "server"'), "duplicate"),
])
def test_parse_rejects(bad, msg):
    with pytest.raises(scenario.ScenarioError, match=msg):
        scenario.parse(bad, Path("x.toml"))


def test_client_needs_server():
    text = GOOD.split("[[process]]")[0] + '[[process]]\nname="c1"\nrole="client"\nargs=[]\n'
    with pytest.raises(scenario.ScenarioError, match="server"):
        scenario.parse(text, Path("x.toml"))


def test_real_scenarios_load_and_cover_ladder():
    all_ = scenario.load_all()
    for step in range(4):
        assert scenario.ladder(step), f"no scenario for step {step}"
        assert scenario.ladder(step, smoke=True), f"no smoke scenario for step {step}"
    assert scenario.find_scenario("selftest-crash").step == 0

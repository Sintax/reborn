import tomllib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Literal

from . import config, netem

ROLES = ("server", "client", "solo")


class ScenarioError(ValueError):
    pass


@dataclass
class ProcessSpec:
    name: str
    role: Literal["server", "client", "solo"]
    args: list[str] = field(default_factory=list)


@dataclass
class Scenario:
    name: str
    step: int
    smoke: bool
    time_limit_s: int
    pass_when: Literal["survive", "match_end"]
    required_passes: int
    expect_map: str | None
    processes: list[ProcessSpec]
    path: Path
    # The map has enemies to fight: a combat-brain player that never fires or casts is a failure.
    expect_combat: bool = False
    # Clients reach the server through the "bad internet" relay (debugloop/netem.py); None = direct.
    network: netem.Impairment | None = None


def parse(text: str, path: Path) -> Scenario:
    try:
        d = tomllib.loads(text)
    except tomllib.TOMLDecodeError as e:
        raise ScenarioError(f"{path}: bad TOML: {e}") from e
    def need(k, t):
        if k not in d or not isinstance(d[k], t):
            raise ScenarioError(f"{path}: missing or wrong type: {k}")
        return d[k]
    name = need("name", str)
    step = need("step", int)
    if not 0 <= step <= 3:
        raise ScenarioError(f"{path}: step must be 0-3, got {step}")
    pass_when = d.get("pass_when", "survive")
    if pass_when not in ("survive", "match_end"):
        raise ScenarioError(f"{path}: pass_when must be survive|match_end")
    expect_combat = d.get("expect_combat", False)
    if not isinstance(expect_combat, bool):
        raise ScenarioError(f"{path}: expect_combat must be true or false")
    network = None
    if "network" in d:
        if not isinstance(d["network"], dict):
            raise ScenarioError(f"{path}: [network] must be a table")
        try:
            network = netem.impairment_from(d["network"])
        except netem.NetworkError as e:
            raise ScenarioError(f"{path}: {e}") from e
    procs = []
    for p in d.get("process", []):
        if p.get("role") not in ROLES:
            raise ScenarioError(f"{path}: bad role {p.get('role')!r}")
        procs.append(ProcessSpec(p["name"], p["role"], list(p.get("args", []))))
    if not procs:
        raise ScenarioError(f"{path}: no [[process]]")
    names = [p.name for p in procs]
    if len(set(names)) != len(names):
        raise ScenarioError(f"{path}: duplicate process name")
    roles = [p.role for p in procs]
    if "client" in roles and roles.count("server") != 1:
        raise ScenarioError(f"{path}: clients need exactly one server")
    return Scenario(name, step, bool(d.get("smoke", False)), need("time_limit_s", int),
                    pass_when, int(d.get("required_passes", 1)), d.get("expect_map"),
                    procs, path, expect_combat, network)


def load(path: Path) -> Scenario:
    return parse(path.read_text(encoding="utf-8"), path)


def load_all(directory: Path = config.SCENARIOS_DIR) -> list[Scenario]:
    return [load(p) for p in sorted(directory.glob("*.toml"))]


def ladder(step: int, smoke: bool = False) -> list[Scenario]:
    return sorted((s for s in load_all() if s.step == step and s.smoke == smoke),
                  key=lambda s: s.name)


def find_scenario(name: str) -> Scenario:
    # selftest/ and net/ (internet-relay runs) are found by name but are not on the ladder.
    for p in list(config.SCENARIOS_DIR.glob("*.toml")) + list(
            (config.SCENARIOS_DIR / "selftest").glob("*.toml")) + list(
            (config.SCENARIOS_DIR / "net").glob("*.toml")):
        s = load(p)
        if s.name == name:
            return s
    raise ScenarioError(f"no scenario named {name}")

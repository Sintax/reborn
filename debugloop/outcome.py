import math
from dataclasses import dataclass, field

from .scenario import Scenario

DESYNC_UNITS = 1500
DESYNC_SAMPLES = 5
STARTUP_TIMEOUT_S = 240


@dataclass
class Sample:
    t: float
    name: str
    state: dict | None
    http_status: int | None


@dataclass
class ProcessRecord:
    name: str
    role: str
    exit_code: int | None
    crash_reports: list[dict] = field(default_factory=list)
    hang_report: dict | None = None
    log_tail: list[str] = field(default_factory=list)


@dataclass
class Outcome:
    kind: str
    detail: str = ""
    process: str | None = None
    phase: str = "startup"
    frame: str | None = None
    code: str | None = None


def _ready(role: str, st: dict | None) -> bool:
    if not st or st.get("ticks", 0) <= 0:
        return False
    if role == "server":
        return bool(st.get("listening"))
    return bool(st.get("has_pawn"))


def _phase(scn: Scenario, samples: list[Sample]) -> tuple[str, int]:
    """Return phase and index of the first sample batch where all were ready."""
    roles = {p.name: p.role for p in scn.processes}
    ready: set[str] = set()
    for i, s in enumerate(samples):
        if _ready(roles.get(s.name, ""), s.state):
            ready.add(s.name)
        if ready >= set(roles):
            return "playing", i
    return "startup", len(samples)


def classify(scn: Scenario, samples: list[Sample], procs: list[ProcessRecord],
             elapsed_s: float, match_ended: bool) -> Outcome:
    phase, start = _phase(scn, samples)
    roles = {p.name: p.role for p in scn.processes}

    # A first-chance report only counts if the process then died: the game (or its
    # anti-tamper) may raise and handle access violations on purpose.
    crashed = [(r, c) for r in procs for c in r.crash_reports
               if r.exit_code is not None or not c.get("first_chance", False)]
    if crashed:
        r, c = crashed[0]
        return Outcome("crash", c.get("address", ""), r.name, phase,
                       "|".join(c.get("frames", [])) or "unknown", c.get("code"))
    for r in procs:
        if r.hang_report:
            return Outcome("hang", f"{r.hang_report.get('seconds')}s", r.name, phase,
                           "|".join(r.hang_report.get("frames", [])) or "unknown")
    for name in roles:
        last = [s for s in samples if s.name == name][-3:]
        if len(last) == 3 and all(s.http_status == 503 for s in last):
            return Outcome("hang", "game_thread_unresponsive", name, phase, "unknown")
    for r in procs:
        if r.exit_code is not None:
            if (r.role == "server" and r.exit_code == 0 and match_ended
                    and scn.pass_when == "match_end"):
                return Outcome("pass", "match_end", r.name, phase)
            return Outcome("exit", "", r.name, phase, code=str(r.exit_code))

    if phase == "playing":
        play = samples[start:]
        n_clients = sum(1 for v in roles.values() if v == "client")
        for s in play:
            if not s.state:
                continue
            if roles.get(s.name) == "client" and s.state.get("connected") is False:
                cat = s.state.get("disconnect_reason") or "unknown"
                return Outcome("disconnect", f"client:{cat}", s.name, phase)
            if roles.get(s.name) == "server" and s.state.get("connections", n_clients) < n_clients:
                return Outcome("disconnect", "server:dropped", s.name, phase)
        d = _desync(roles, play)
        if d:
            return Outcome("desync", d, d, phase)
        for s in play:
            if (scn.expect_map and s.state and s.state.get("map")
                    and s.state["map"] != scn.expect_map):
                return Outcome("exit", f"wrong_map {s.state['map']}", s.name, phase,
                               code="wrong_map")
    elif elapsed_s >= STARTUP_TIMEOUT_S:
        return Outcome("timeout", "", None, "startup")

    if elapsed_s >= scn.time_limit_s:
        if scn.pass_when == "survive" and phase == "playing":
            return Outcome("pass", "survived", None, phase)
        return Outcome("timeout", "", None, phase)
    return Outcome("running", "", None, phase)


def _desync(roles: dict[str, str], play: list[Sample]) -> str | None:
    server_name = next((n for n, r in roles.items() if r == "server"), None)
    if not server_name:
        return None
    streak: dict[str, int] = {}
    last_server: dict | None = None
    for s in play:
        if s.name == server_name:
            last_server = s.state
            continue
        if roles.get(s.name) != "client" or not s.state or not last_server:
            continue
        mine = s.state.get("pawn_location")
        theirs = (last_server.get("player_locations") or {}).get(s.name)
        if not mine or not theirs:
            continue
        if math.dist(mine, theirs) > DESYNC_UNITS:
            streak[s.name] = streak.get(s.name, 0) + 1
            if streak[s.name] >= DESYNC_SAMPLES:
                return s.name
        else:
            streak[s.name] = 0
    return None

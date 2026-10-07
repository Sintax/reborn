import math
import re
from dataclasses import dataclass, field

from . import config
from .scenario import Scenario

DESYNC_UNITS = 1500
DESYNC_SAMPLES = 5
# A player who keeps falling out of the world (no floor or no collision) dies and respawns about
# every 30 s. Stepping off a ledge now and then is normal; this many falls this fast is not.
FALL_UNITS = 5000
FALL_COUNT = 4
FALL_WINDOW_S = 300
# A player whose body is not drawn (its pawn never got its skin's mesh). A new pawn has no mesh until
# its skin is applied, so the first INVISIBLE_GRACE_S after it appears are skipped; after that it
# has to stay invisible for INVISIBLE_PERSIST_S over at least INVISIBLE_SAMPLES samples. The same
# timing judges a pawn of the wrong hero (wronghero) or in another hero's skin (wrongskin).
INVISIBLE_GRACE_S = 30
INVISIBLE_PERSIST_S = 60
INVISIBLE_SAMPLES = 3
STARTUP_TIMEOUT_S = 240
# On a map with enemies (Scenario.expect_combat), a combat-brain player that has been in the game
# this long without one shot or skill is not fighting at all.
NOCOMBAT_PLAY_S = 180
WATCHDOG_GRACE_S = 10    # time the watchdog gets to write its dump and report


@dataclass
class Sample:
    t: float
    name: str
    state: dict | None
    http_status: int | None       # None: no HTTP answer at all (refused or timed out)
    alive: bool | None = None     # process still running at this poll; None = not recorded


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


# Autopilot phases (the `autopilot` field of a client's or solo game's /state), in the order a
# join goes through them. Anything else (missing, "off", "waiting_for_menu", unknown) scores 0.
AUTOPILOT_MILESTONE = {"menu_ready": 1, "launching": 2, "character_select": 3, "playing": 4}


def milestone(samples: list[Sample]) -> int:
    """How far a run got, 0-6: the furthest autopilot phase any client (or the solo game) reached,
    plus one if the server ever reported a connection, plus one if the server ever reported a
    player location (it spawned a pawn for a joined player, which the client may not see yet).
    Only ever read from the samples, so it does not depend on how the run ended."""
    best, connected, spawned = 0, False, False
    for s in samples:
        st = s.state
        if not isinstance(st, dict):
            continue
        ph = st.get("autopilot")
        if isinstance(ph, str):
            best = max(best, AUTOPILOT_MILESTONE.get(ph, 0))
        n = st.get("connections")
        locs = st.get("player_locations")
        if (isinstance(n, int) and not isinstance(n, bool) and n > 0) or locs:
            connected = True
        if isinstance(locs, dict) and locs and (s.name == "server" or st.get("role") == "server"):
            spawned = True
    return best + connected + spawned


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
    o = _classify(scn, samples, procs, elapsed_s, match_ended)
    if o.kind == "pass" and scn.expect_combat:
        idle = _never_fought(scn, samples)
        if idle:
            o = Outcome("nocombat", idle[1], idle[0], "playing")
    # Outcomes decided at or after the desync check get a note when the check was blind.
    if o.kind in ("desync", "pass", "timeout", "running") or o.code == "wrong_map":
        phase, start = _phase(scn, samples)
        if phase == "playing":
            roles = {p.name: p.role for p in scn.processes}
            missing = _missing_unique_id(roles, samples[start:])
            if missing:
                note = "no unique_id from " + ",".join(missing) + " (desync matched by name)"
                o.detail = f"{o.detail}; {note}" if o.detail else note
    return o


def _never_fought(scn: Scenario, samples: list[Sample]) -> tuple[str, str] | None:
    """(process, detail) for a player with the combat brain on that was in the game (a pawn, or
    autopilot "playing") for NOCOMBAT_PLAY_S or more, yet its last stats show no shot and no skill.
    Samples without a combat block (older mod builds, brain off) never count."""
    roles = {p.name: p.role for p in scn.processes}
    first_play: dict[str, float] = {}
    last_t: dict[str, float] = {}
    combat: dict[str, dict] = {}
    for s in samples:
        if roles.get(s.name) not in ("client", "solo") or not isinstance(s.state, dict):
            continue
        st = s.state
        if st.get("has_pawn") or st.get("autopilot") == "playing":
            first_play.setdefault(s.name, s.t)
        last_t[s.name] = s.t
        if isinstance(st.get("combat"), dict):
            combat[s.name] = st["combat"]
    for name, c in combat.items():
        played = last_t[name] - first_play.get(name, last_t[name])
        stats = c.get("stats") if isinstance(c.get("stats"), dict) else {}
        if (c.get("enabled") and played >= NOCOMBAT_PLAY_S
                and not stats.get("shots") and not stats.get("skills_used")):
            return name, (f"{name} had the combat brain on for {int(played)} s but never fired or "
                          "used a skill; on this map it should find and fight enemies")
    return None


def _missing_unique_id(roles: dict[str, str], play: list[Sample]) -> list[str]:
    """Clients that reported a pawn_location but no unique_id."""
    return sorted({s.name for s in play
                   if roles.get(s.name) == "client" and s.state
                   and s.state.get("pawn_location") and not s.state.get("unique_id")})


def _silent(s: Sample, running: bool) -> bool:
    """A poll the game did not answer: a 503, or no HTTP answer at all from a process that is
    still running. A process that has exited is never 'silent', just gone."""
    if s.http_status == 503:
        return True
    return s.http_status is None and running and s.alive is not False


def _not_answering(name: str, samples: list[Sample], running: bool) -> bool:
    """The latest poll went unanswered after the game had ticked: it may be frozen, so no pass yet."""
    mine = [s for s in samples if s.name == name]
    return (bool(mine) and _silent(mine[-1], running)
            and any(s.state and s.state.get("ticks", 0) > 0 for s in mine))


def _unresponsive(name: str, samples: list[Sample], running: bool) -> bool:
    """Last three polls went unanswered. Before the first tick the game is still loading and
    answers 503 too; the startup timeout covers that."""
    mine = [s for s in samples if s.name == name]
    ticked = any(s.state and s.state.get("ticks", 0) > 0 for s in mine)
    last = mine[-3:]
    return ticked and len(last) == 3 and all(_silent(s, running) for s in last)


HANG_SECONDS_RANGE = (5, 3600)   # LaunchOptions.cpp: -rbhangsecs outside this is ignored


def _watchdog_limit(scn: Scenario, name: str) -> int:
    """The in-game freeze limit, parsed the way LaunchOptions does: exact flag name, a whole
    number in range, the last valid one wins, anything else leaves the default."""
    spec = next((p for p in scn.processes if p.name == name), None)
    limit = config.HANG_SECONDS
    for a in (spec.args if spec else []):
        key, _, val = a.partition("=")
        if key != "-rbhangsecs":
            continue
        # std::stoi skips leading spaces and takes a sign; the mod then rejects any trailing text.
        if re.fullmatch(r"\s*[+-]?\d+", val) and HANG_SECONDS_RANGE[0] <= int(val) <= HANG_SECONDS_RANGE[1]:
            limit = int(val)
    return limit


def _overtime_cap(scn: Scenario) -> float:
    """Longest a run may go past time_limit_s while waiting for a freeze to be decided."""
    return max(_watchdog_limit(scn, p.name) for p in scn.processes) + WATCHDOG_GRACE_S


def _fault_not_outlived(c: dict, name: str, samples: list[Sample]) -> bool:
    """A first-chance fault the game survived: some later sample shows ticks past the fault's tick.
    Reports without a tick count (older mod builds) are taken at face value."""
    at = c.get("ticks")
    if not isinstance(at, (int, float)):
        return True
    return not any(s.name == name and s.state and s.state.get("ticks", 0) > at for s in samples)


def _frozen_past_watchdog(scn: Scenario, name: str, samples: list[Sample], running: bool) -> bool:
    """Unresponsive for longer than the in-game watchdog needs to write its own report (with frames)."""
    if not _unresponsive(name, samples, running):
        return False
    mine = [s for s in samples if s.name == name]
    last_ok = max((s.t for s in mine if s.state and s.state.get("ticks", 0) > 0), default=None)
    return last_ok is not None and mine[-1].t - last_ok >= _watchdog_limit(scn, name) + WATCHDOG_GRACE_S


def _classify(scn: Scenario, samples: list[Sample], procs: list[ProcessRecord],
              elapsed_s: float, match_ended: bool) -> Outcome:
    phase, start = _phase(scn, samples)
    roles = {p.name: p.role for p in scn.processes}

    # Processes with no exit code yet; a process that exited is never counted as "not answering".
    running = {r.name for r in procs if r.exit_code is None} | (set(roles) - {r.name for r in procs})
    unresponsive = {name for name in roles if _unresponsive(name, samples, name in running)}
    # A first-chance report only counts if the process exited with non-zero code: the
    # game (or its anti-tamper) may raise and handle access violations on purpose. A game-thread
    # fault also counts when the game thread then stops: the game's own handler catches the
    # fault, logs "Critical error" and sits there without exiting, so no final report comes.
    # A fault the game kept ticking past was handled, so it cannot explain a later freeze.
    def counts(r: ProcessRecord, c: dict) -> bool:
        if not c.get("first_chance", False) or (r.exit_code is not None and r.exit_code != 0):
            return True
        return (bool(c.get("game_thread")) and (r.name in unresponsive or r.hang_report is not None)
                and _fault_not_outlived(c, r.name, samples))
    crashed = [(r, c) for r in procs for c in r.crash_reports if counts(r, c)]
    if crashed:
        # Prefer the final (second-chance) report: it is the one that actually killed the process.
        crashed.sort(key=lambda rc: bool(rc[1].get("first_chance", False)))
        r, c = crashed[0]
        return Outcome("crash", c.get("address", ""), r.name, phase,
                       "|".join(c.get("frames", [])) or "unknown", c.get("code"))
    for r in procs:
        if r.hang_report:
            return Outcome("hang", f"{r.hang_report.get('seconds')}s", r.name, phase,
                           "|".join(r.hang_report.get("frames", [])) or "unknown")
    for name in roles:
        if _frozen_past_watchdog(scn, name, samples, name in running):
            return Outcome("hang", "game_thread_unresponsive", name, phase, "unknown")
    # Check for any non-zero exit first (must fail the pass check)
    for r in procs:
        if r.exit_code is not None and r.exit_code != 0:
            return Outcome("exit", "", r.name, phase, code=str(r.exit_code))
    # Never pass while a process may be frozen: keep sampling until the freeze is decided.
    not_answering = [name for name in roles if _not_answering(name, samples, name in running)]
    # Check server clean exit (exit 0 + match_ended). A match the game ends by its own rules (won,
    # or lost to minions) is a normal end for every scenario, not only pass_when="match_end".
    # Only if no other process has non-zero exit.
    match_pass = next((r for r in procs if r.role == "server" and r.exit_code == 0 and match_ended),
                      None)
    if match_pass and not not_answering:
        return Outcome("pass", "match_end", match_pass.name, phase)
    # Check for any zero exit (a match-end pass on hold for a silent client waits below instead)
    for r in procs:
        if r.exit_code is not None and not match_pass:
            return Outcome("exit", "", r.name, phase, code=str(r.exit_code))

    if match_pass:
        pass   # the match is over: post-match disconnects and map changes are expected, not failures
    elif phase == "playing":
        play = _before_match_end(roles, samples[start:])
        n_clients = sum(1 for v in roles.values() if v == "client")
        for s in play:
            if not s.state:
                continue
            if roles.get(s.name) == "client" and s.state.get("connected") is False:
                cat = s.state.get("disconnect_reason") or "unknown"
                return Outcome("disconnect", f"client:{cat}", s.name, phase)
            if roles.get(s.name) == "server" and s.state.get("connections", n_clients) < n_clients:
                return Outcome("disconnect", "server:dropped", s.name, phase)
        f = _keeps_falling(roles, play)
        if f:
            return Outcome("fell", f, f, phase)
        # The wrong hero or skin before invisible: a pawn in another hero's skin often has no body
        # either, and the skin is the cause worth naming.
        for kind, judge in (("wronghero", _wrong_hero), ("wrongskin", _wrong_skin),
                            ("invisible", _invisible)):
            bad = judge(roles, play)
            if bad:
                return Outcome(kind, bad[1], bad[0], phase)
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

    if not_answering and (elapsed_s >= scn.time_limit_s or match_ended):
        # Past the limit the run gets up to the watchdog limit + grace to show whether it is frozen.
        if elapsed_s >= scn.time_limit_s + _overtime_cap(scn):
            return Outcome("hang", "game_thread_unresponsive", not_answering[0], phase, "unknown")
        return Outcome("running", "", None, phase)
    if elapsed_s >= scn.time_limit_s:
        if scn.pass_when == "survive" and phase == "playing":
            return Outcome("pass", "survived", None, phase)
        return Outcome("timeout", "", None, phase)
    return Outcome("running", "", None, phase)


def _before_match_end(roles: dict[str, str], play: list[Sample]) -> list[Sample]:
    """Samples up to the server's last report from before the match ended. After the game ends the
    match the server drops everyone and clients go back to the menu, which is not a failure.
    Any process can say the match is over: a client shows Victory/Defeat (and stops its pawn)
    several seconds before the server logs "Match ended"."""
    over = next((s.t for s in play if s.state and s.state.get("match_over")), None)
    if over is None:
        return play
    last_running = max((s.t for s in play if roles.get(s.name) == "server" and s.state
                        and not s.state.get("match_over") and s.t < over), default=None)
    return [s for s in play if last_running is not None and s.t <= last_running]


def _keeps_falling(roles: dict[str, str], play: list[Sample]) -> str | None:
    """A player whose pawn drops FALL_UNITS below where it first stood, FALL_COUNT times within
    FALL_WINDOW_S: the floor is missing for them (seen on Meltdown, run 20261005-054440)."""
    start: dict[str, float] = {}
    below: dict[str, bool] = {}
    falls: dict[str, list[float]] = {}
    for s in play:
        if roles.get(s.name) not in ("client", "solo") or not s.state:
            continue
        loc = s.state.get("pawn_location")
        if not loc:
            continue
        try:
            z = float(loc[2])
        except (TypeError, ValueError, IndexError):
            continue
        start.setdefault(s.name, z)
        low = z < start[s.name] - FALL_UNITS
        if low and not below.get(s.name):
            times = [t for t in falls.get(s.name, []) if s.t - t <= FALL_WINDOW_S] + [s.t]
            falls[s.name] = times
            if len(times) >= FALL_COUNT:
                return s.name
        below[s.name] = low
    return None


def _invisible_count(v) -> int:
    """others_invisible is a list of "name (hero): what is missing"; a bare count is accepted too."""
    if isinstance(v, list):
        return len(v)
    if isinstance(v, int) and not isinstance(v, bool):
        return max(v, 0)
    return 0


def _listed(v) -> str:
    return "; ".join(map(str, v)) if isinstance(v, list) else str(v)


def _persists(roles: dict[str, str], play: list[Sample], checks) -> tuple[str, str] | None:
    """(process, detail) for the first check that stays bad on a living player's pawn for
    INVISIBLE_PERSIST_S over INVISIBLE_SAMPLES samples, after the pawn's first INVISIBLE_GRACE_S
    (a new pawn gets its skin, body and class a little after it appears). checks(state) gives
    {name: (bad, detail)}. A dead player is skipped and starts over when it respawns."""
    appeared: dict[str, float] = {}
    bad_since: dict[tuple[str, str], tuple[float, int]] = {}   # (process, check) -> (first t, samples)
    for s in play:
        if roles.get(s.name) not in ("client", "solo") or not s.state:
            continue
        st = s.state
        health = st.get("pawn_health")
        dead = isinstance(health, (int, float)) and not isinstance(health, bool) and health <= 0
        if not st.get("has_pawn") or dead:
            appeared.pop(s.name, None)
            for key in [k for k in bad_since if k[0] == s.name]:
                del bad_since[key]
            continue
        start = appeared.setdefault(s.name, s.t)
        for what, (bad, detail) in checks(st).items():
            key = (s.name, what)
            if not bad or s.t - start < INVISIBLE_GRACE_S:
                bad_since.pop(key, None)
                continue
            first, n = bad_since.get(key, (s.t, 0))
            bad_since[key] = (first, n + 1)
            if s.t - first >= INVISIBLE_PERSIST_S and n + 1 >= INVISIBLE_SAMPLES:
                return s.name, f"{s.name}: {detail}"
    return None


def _invisible(roles: dict[str, str], play: list[Sample]) -> tuple[str, str] | None:
    """A player whose body is not drawn: a client's own pawn has no visible mesh (pawn_visible
    false), or a client sees another human player's pawn without one (others_invisible). Seen on
    Meltdown, run 20261006-023201: player 2's pawn never got its skin. Samples without these
    fields (older mod builds) never count."""
    def checks(st):
        others = st.get("others_invisible")
        return {
            "own": (st.get("pawn_visible") is False,
                    f"own pawn has no visible body ({st.get('pawn_body_missing') or 'unknown'})"),
            "others": (_invisible_count(others) > 0,
                       "sees players without a visible body: " + _listed(others)),
        }
    return _persists(roles, play, checks)


def _wrong_hero(roles: dict[str, str], play: list[Sample]) -> tuple[str, str] | None:
    """A player whose pawn is not the hero it picked (hero_matches false). Samples with no pick
    or no answer (null, or an older mod build without the field) never count."""
    def checks(st):
        return {"hero": (st.get("hero_matches") is False,
                         f"picked {st.get('requested_hero') or '?'} but plays "
                         f"{st.get('pawn_hero') or '?'}")}
    return _persists(roles, play, checks)


def _wrong_skin(roles: dict[str, str], play: list[Sample]) -> tuple[str, str] | None:
    """A player whose own pawn wears another hero's skin (pawn_skin_ok false), or who sees another
    human player in one (others_wrong_skin). That is how the s3 ModernSoldier lost its body
    (runs 20261006-023201, -025246). Samples without these fields never count."""
    def checks(st):
        others = st.get("others_wrong_skin")
        return {
            "own": (st.get("pawn_skin_ok") is False,
                    f"own pawn ({st.get('pawn_hero') or '?'}) wears another hero's skin "
                    f"({st.get('pawn_skin_wrong') or 'unknown'})"),
            "others": (_invisible_count(others) > 0,
                       "sees players in another hero's skin: " + _listed(others)),
        }
    return _persists(roles, play, checks)


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
        # The server keys player_locations by each player's UniqueId; fall back to the instance name.
        key = s.state.get("unique_id") or s.name
        theirs = (last_server.get("player_locations") or {}).get(key)
        if not mine or not theirs:
            continue
        # A dead player's body is not kept in step: the client's corpse stays put while the
        # server's copy can keep falling (run 20261005-105110, "Death Recap" on screen).
        health = s.state.get("pawn_health")
        if isinstance(health, (int, float)) and health <= 0:
            streak[s.name] = 0
            continue
        try:
            if math.dist(mine, theirs) > DESYNC_UNITS:
                streak[s.name] = streak.get(s.name, 0) + 1
                if streak[s.name] >= DESYNC_SAMPLES:
                    return s.name
            else:
                streak[s.name] = 0
        except (TypeError, ValueError):
            # Skip malformed location lists
            continue
    return None

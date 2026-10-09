"""Do the server and each client agree on the computer-controlled characters?

The mod's /state lists every networked non-player pawn (story enemies, lane minions, pets) and every
networked thing a player made (summons, mines, deployables) as "npcs": archetype, location, health,
owner and what keeps its body from being drawn. Each poll round, for each client:

  missing    the server has it within RELEVANT_UNITS of the client's player; the client has no
             entry of the same archetype within MATCH_UNITS of it
  ghost      the client has it near its player; the server has nothing of that archetype nearby
  invisible  both have it, but the client's copy has no body to draw

One round proves nothing (packets in flight, a pawn just spawned or just died), so findings are
followed over time: a finding continues an episode when the next round has the same kind and
archetype within FOLLOW_UNITS of where it was. An episode lasting PERSIST_S or more is a failure.
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field

RELEVANT_UNITS = 10000.0  # within the game's usual network range (UE3 default cull distance 15000)
MATCH_UNITS = 700.0       # how far apart the two copies may be (movement during the poll, lag)
FOLLOW_UNITS = 1500.0     # how far a finding may move between rounds and still be the same one
PERSIST_S = 12.0
PAIR_S = 3.0              # a client sample counts with the server sample at most this far apart in time
KINDS = ("missing", "ghost", "invisible")


def _dist(a, b) -> float:
    return math.dist(a, b)


def _alive(e: dict) -> bool:
    return e.get("k") != "pawn" or e.get("hp", 1) > 0


@dataclass
class Episode:
    client: str
    kind: str
    archetype: str
    cls: str
    first_t: float
    last_t: float
    pos: list
    rounds: int = 1
    detail: str = ""

    @property
    def seconds(self) -> float:
        return self.last_t - self.first_t

    def describe(self) -> str:
        what = {"missing": "the server has it near this player, the player's game does not",
                "ghost": "this player's game shows it, the server has no such thing there",
                "invisible": "this player's game has it but draws no body"}[self.kind]
        extra = f" ({self.detail})" if self.detail else ""
        return (f"{self.client}: {self.kind} {self.archetype} ({self.cls}) for {self.seconds:.0f} s "
                f"near ({', '.join(str(int(v)) for v in self.pos)}): {what}{extra}")


def compare_round(server_npcs: list[dict], client_npcs: list[dict], me: list | None) -> dict[str, list[dict]]:
    """One round's findings for one client: {kind: [npc entries]}."""
    out = {k: [] for k in KINDS}
    if not me:
        return out
    srv = [e for e in server_npcs if _alive(e) and e.get("p")]
    cli = [e for e in client_npcs if _alive(e) and e.get("p")]
    used: set[int] = set()
    for s in sorted(srv, key=lambda e: _dist(e["p"], me)):
        # Player-made things (bombs, mines: PoplarServerSideProjectile) mostly live on the server
        # only, by design; the client draws an effect. Only characters must be on both.
        if s.get("k") != "pawn" or _dist(s["p"], me) > RELEVANT_UNITS:
            continue
        best, best_d = None, MATCH_UNITS
        for i, c in enumerate(cli):
            if i in used or c.get("a") != s.get("a"):
                continue
            d = _dist(c["p"], s["p"])
            if d <= best_d:
                best, best_d = i, d
        if best is None:
            out["missing"].append(s)
        else:
            used.add(best)
            if cli[best].get("body"):
                out["invisible"].append(cli[best])
    for i, c in enumerate(cli):
        if i in used or c.get("k") != "pawn" or _dist(c["p"], me) > RELEVANT_UNITS:
            continue
        if not any(s.get("a") == c.get("a") and _dist(s["p"], c["p"]) <= MATCH_UNITS for s in srv):
            out["ghost"].append(c)
    return out


@dataclass
class Report:
    episodes: list[Episode] = field(default_factory=list)
    rounds: dict[str, int] = field(default_factory=dict)        # client -> rounds compared
    compared: dict[str, int] = field(default_factory=dict)      # client -> server entries near the player
    archetypes: dict[str, int] = field(default_factory=dict)    # archetype -> most seen at once on the server

    def failures(self, persist_s: float = PERSIST_S) -> list[Episode]:
        return sorted((e for e in self.episodes if e.seconds >= persist_s), key=lambda e: -e.seconds)

    def to_json(self, persist_s: float = PERSIST_S) -> dict:
        return {"rounds": self.rounds, "compared": self.compared, "archetypes": self.archetypes,
                "failures": [e.describe() for e in self.failures(persist_s)],
                "episodes": [{"client": e.client, "kind": e.kind, "archetype": e.archetype, "class": e.cls,
                              "seconds": round(e.seconds, 1), "rounds": e.rounds, "pos": e.pos,
                              "detail": e.detail} for e in self.episodes]}


def analyse(samples, server: str = "server") -> Report:
    """samples: run.Sample-like objects (t, name, state) in time order."""
    rep = Report()
    srv_samples = [s for s in samples if s.name == server and s.state and isinstance(s.state.get("npcs"), list)]
    open_eps: dict[str, list[Episode]] = {}
    for s in samples:
        if s.name == server or not s.state or not isinstance(s.state.get("npcs"), list):
            continue
        if not s.state.get("has_pawn") or not s.state.get("pawn_location"):
            continue
        srv = min(srv_samples, key=lambda x: abs(x.t - s.t), default=None)
        if srv is None or abs(srv.t - s.t) > PAIR_S:
            continue
        counts: dict[str, int] = {}
        for e in srv.state["npcs"]:
            counts[e.get("a", "?")] = counts.get(e.get("a", "?"), 0) + 1
        for a, n in counts.items():
            rep.archetypes[a] = max(rep.archetypes.get(a, 0), n)
        me = s.state["pawn_location"]
        found = compare_round(srv.state["npcs"], s.state["npcs"], me)
        rep.rounds[s.name] = rep.rounds.get(s.name, 0) + 1
        rep.compared[s.name] = rep.compared.get(s.name, 0) + sum(
            1 for e in srv.state["npcs"] if e.get("k") == "pawn" and _alive(e) and e.get("p")
            and _dist(e["p"], me) <= RELEVANT_UNITS)
        prev = open_eps.get(s.name, [])
        now_open: list[Episode] = []
        for kind, entries in found.items():
            for e in entries:
                ep = next((p for p in prev if p.kind == kind and p.archetype == e.get("a")
                           and p not in now_open and _dist(p.pos, e["p"]) <= FOLLOW_UNITS), None)
                if ep is None:
                    ep = Episode(s.name, kind, e.get("a", "?"), e.get("c", "?"), s.t, s.t, list(e["p"]),
                                 detail=e.get("body", "") if kind == "invisible" else "")
                    rep.episodes.append(ep)
                else:
                    ep.last_t, ep.pos, ep.rounds = s.t, list(e["p"]), ep.rounds + 1
                now_open.append(ep)
        open_eps[s.name] = now_open
    return rep

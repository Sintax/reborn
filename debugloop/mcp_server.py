"""MCP server "battleborn-play": lets Claude look at a running test game and give it orders.

A thin wrapper over debugloop.play. Errors come back as text, never as exceptions, so the
transport stays up and Claude reads "c2: no_pawn" instead of a traceback. It never starts or stops
a game; that is `python -m debugloop.loop play` / `stop-play`.
"""
from __future__ import annotations

import functools
import json
import sys

from mcp.server.mcpserver import Image, MCPServer

from . import play


def _guard(fn):
    @functools.wraps(fn)
    def run(*a, **k):
        try:
            return fn(*a, **k)
        except play.PlayError as e:
            return f"error: {e}"
    return run


@_guard
def bb_list() -> str:
    """List the running test games (name, role, debug port, pid). Names are what the other tools take."""
    found = play.discover()
    if not found:
        return "no test games running (start one with: python -m debugloop.loop play <scenario>)"
    return "\n".join(f"{i.name} ({i.role}) port {i.port} pid {i.pid}" for i in found)


@_guard
def bb_situation(instance: str) -> str:
    """Short text for one player: hero, health, position, mode, target, skills, the enemies it knows
    about (id, distance, bearing in degrees with + to the right, visible or hidden) and its stats."""
    return play.situation_text(play.resolve(instance))


@_guard
def bb_combat(instance: str) -> str:
    """Raw combat JSON: everything bb_situation summarises plus enemy locations and the current order."""
    return json.dumps(play.combat(play.resolve(instance)), indent=1)


@_guard
def bb_state(instance: str) -> str:
    """Raw game state JSON: map, role, pawn, match state, memory, combat stats."""
    return json.dumps(play.state(play.resolve(instance)), indent=1)


@_guard
def bb_look(instance: str, max_width: int = 480) -> Image | str:
    """A picture of that player's screen, scaled to max_width pixels. Keep it small (default 480):
    pictures cost far more than bb_situation, so look only when the text leaves a doubt."""
    inst = play.resolve(instance)
    data = play.look(inst, max_width)
    if data is None:
        return f"{inst.name}: no window to capture (is the game minimised or gone?)"
    return Image(data=data, format="png")


@_guard
def bb_order(instance: str, mode: str | None = None, target: str | None = None,
             point: list[float] | None = None, fire: bool | None = None, skills: bool | None = None,
             enabled: bool | None = None) -> str:
    """Set the player's standing order. mode: hunt (find and fight, the default), hold (stand and
    shoot), goto (walk to point [x,y,z]), follow (stay 600-1200 units from target), retreat (back away
    5 s), wander (old random walk). target: an enemy id from bb_situation. fire / skills: allow shooting
    / abilities. enabled: turn the combat brain on or off. Omitted fields keep their value."""
    j = play.order(play.resolve(instance), mode=mode, target=target, point=point, fire=fire,
                   skills=skills, enabled=enabled)
    return json.dumps(j.get("order", j))


@_guard
def bb_act(instance: str, action: str, duration_s: float | None = None) -> str:
    """One-off action: jump, fire_start, fire_stop, fire_burst (for duration_s, default 0.5), altfire,
    melee, skill1, skill2, ultimate, use, sprint_start, sprint_stop."""
    return json.dumps(play.act(play.resolve(instance), action, duration_s))


@_guard
def bb_exec(instance: str, command: str) -> str:
    """Send a raw game console command (e.g. "Jump"). Prefer bb_act when it covers the need."""
    return play.exec_(play.resolve(instance), command)


@_guard
def bb_log(instance: str, lines: int = 40) -> str:
    """The last lines of that game's log ([COMBAT] lines show targets, modes, skills)."""
    return play.log(play.resolve(instance), lines)


TOOLS = (bb_list, bb_situation, bb_combat, bb_state, bb_look, bb_order, bb_act, bb_exec, bb_log)


def build_server() -> MCPServer:
    srv = MCPServer("battleborn-play", instructions=(
        "Drive running Battleborn test games. Start with bb_list, then bb_situation per player. "
        "Give standing orders with bb_order; the in-game brain aims and shoots by itself. "
        "Use bb_look sparingly."))
    for fn in TOOLS:
        srv.tool()(fn)
    return srv


def main() -> int:
    build_server().run("stdio")
    return 0


if __name__ == "__main__":
    sys.exit(main())

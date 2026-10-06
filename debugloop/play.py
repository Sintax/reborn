"""Talk to a running test game: read its combat situation, give orders, take a small picture.

Library for the MCP server (mcp_server.py) and a CLI (`python -m debugloop.play ...`). Only ever
talks to 127.0.0.1. Never starts or stops a game: runs and play sessions own the processes.
"""
from __future__ import annotations

import argparse
import http.client
import io
import json
import sys
import tempfile
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path

from . import config, screenshot

DEFAULT_PORTS = range(config.FIRST_DEBUG_PORT, config.FIRST_DEBUG_PORT + 10)
ACTIONS = ("jump", "fire_start", "fire_stop", "fire_burst", "altfire", "melee", "skill1", "skill2",
           "ultimate", "use", "sprint_start", "sprint_stop")
MODES = ("hunt", "hold", "goto", "follow", "retreat", "wander")


class PlayError(Exception):
    pass


@dataclass
class Instance:
    name: str
    role: str
    port: int
    pid: int


def _request(inst: Instance, path: str, body: dict | str | None = None, timeout: float = 6.0) -> str:
    data = None
    if body is not None:
        data = (body if isinstance(body, str) else json.dumps(body)).encode()
    req = urllib.request.Request(f"http://127.0.0.1:{inst.port}{path}", data=data,
                                 method="POST" if data is not None else "GET")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        raw = e.read().decode("utf-8", "replace")
        try:
            word = json.loads(raw).get("error", raw)
        except (ValueError, AttributeError):
            word = raw or str(e.code)
        raise PlayError(f"{inst.name}: {word}") from None
    except (urllib.error.URLError, TimeoutError, ConnectionError, http.client.HTTPException) as e:
        raise PlayError(f"{inst.name}: no answer from port {inst.port} ({e})") from None


def _json(inst: Instance, path: str, body=None, timeout: float = 6.0) -> dict:
    try:
        return json.loads(_request(inst, path, body, timeout))
    except ValueError:
        raise PlayError(f"{inst.name}: {path} did not return JSON") from None


def discover(ports=DEFAULT_PORTS, timeout: float = 0.5) -> list[Instance]:
    """The test games answering on these ports, as their /state names them."""
    found = []
    for port in ports:
        try:
            st = _json(Instance(f"port {port}", "?", port, 0), "/state", timeout=timeout)
        except PlayError:
            continue
        found.append(Instance(str(st.get("instance", f"port {port}")), str(st.get("role", "?")), port,
                              int(st.get("pid", 0))))
    return found


def resolve(which, instances: list[Instance] | None = None) -> Instance:
    """An instance by name ("c1") or debug port (18081)."""
    instances = instances if instances is not None else discover()
    for i in instances:
        if i.name == str(which) or str(i.port) == str(which):
            return i
    names = ", ".join(f"{i.name} ({i.port})" for i in instances) or "none running"
    raise PlayError(f"no instance '{which}'; running: {names}")


def state(inst: Instance) -> dict:
    return _json(inst, "/state")


def combat(inst: Instance) -> dict:
    return _json(inst, "/combat")


def order(inst: Instance, mode=None, target=None, point=None, fire=None, skills=None, enabled=None,
          tuning=None) -> dict:
    """Change the standing order; fields left as None keep their value in the game."""
    body = {k: v for k, v in {"mode": mode, "target": target, "point": point, "fire": fire,
                              "skills": skills, "enabled": enabled, "tuning": tuning}.items() if v is not None}
    return _json(inst, "/order", body)


def act(inst: Instance, action: str, duration_s: float | None = None) -> dict:
    body: dict = {"action": action}
    if duration_s is not None:
        body["duration_s"] = duration_s
    return _json(inst, "/act", body)


def exec_(inst: Instance, command: str) -> str:
    return _request(inst, "/exec", command)


def log(inst: Instance, lines: int = 40) -> str:
    return "\n".join(_request(inst, "/log").splitlines()[-lines:])


def look(inst: Instance, max_width: int = 480) -> bytes | None:
    """A PNG of the game window, scaled down to max_width. None when there is no window."""
    from PIL import Image
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / "shot.jpg"
        if not inst.pid or not screenshot.capture(inst.pid, p):
            return None
        with Image.open(p) as src:
            img = src.convert("RGB")
    if img.width > max_width:
        img = img.resize((max_width, max(1, round(img.height * max_width / img.width))))
    out = io.BytesIO()
    img.save(out, "PNG")
    return out.getvalue()


def situation_text(inst: Instance) -> str:
    """About 15 lines: who I am, what I am doing, what I know about enemies, how it is going."""
    c = combat(inst)
    me = c.get("me", {})
    loc = me.get("location") or [0, 0, 0]
    skills = ", ".join(f"{s['slot']} ready" if s.get("ready") else f"{s['slot']} in {s.get('cooldown_s', 0):.1f}s"
                       for s in me.get("skills", []))
    lines = [
        f"{inst.name}: {me.get('hero', '?')} hp {me.get('health', 0):.0f}/{me.get('max_health', 0):.0f}"
        f"{' DEAD' if me.get('dead') else ''} at ({loc[0]:.0f}, {loc[1]:.0f}, {loc[2]:.0f}) yaw {me.get('yaw', 0):.0f}",
        f"mode {c.get('mode')}  target {c.get('target') or 'none'}  aim {'on' if c.get('aim_on') else 'off'}"
        f"  firing {'yes' if c.get('firing') else 'no'}  combat {'on' if c.get('enabled') else 'OFF'}",
        f"skills: {skills or 'unknown'}",
    ]
    enemies = c.get("enemies", [])
    lines.append(f"enemies known: {c.get('n_enemies', len(enemies))}")
    for e in enemies[:8]:
        mark = "*" if e.get("id") == c.get("target") else "-"
        lines.append(f"{mark} {e['id']} {e['hero']} {e['kind']} {e['distance']:.0f}u bearing {e['bearing']:.0f}"
                     f" {'visible' if e.get('visible') else 'hidden'} hp {e['health']:.0f}")
    s = c.get("stats", {})
    lines.append(f"stats: shots {s.get('shots', 0)} skills {s.get('skills_used', 0)} kills {s.get('kills', 0)}"
                 f" deaths {s.get('deaths', 0)} dmg taken {s.get('damage_taken', 0):.0f}")
    return "\n".join(lines)


def _ports(spec: str | None):
    if not spec:
        return DEFAULT_PORTS
    lo, _, hi = spec.partition("-")
    return range(int(lo), int(hi or lo) + 1)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="python -m debugloop.play")
    ap.add_argument("--ports", help="port range to scan, e.g. 18080-18089")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    for name in ("state", "combat", "situation", "log"):
        sub.add_parser(name).add_argument("instance")
    o = sub.add_parser("order")
    o.add_argument("instance")
    o.add_argument("mode", choices=MODES)
    o.add_argument("--target")
    o.add_argument("--point", nargs=3, type=float)
    o.add_argument("--no-fire", action="store_true")
    o.add_argument("--no-skills", action="store_true")
    a = sub.add_parser("act")
    a.add_argument("instance")
    a.add_argument("action", choices=ACTIONS)
    a.add_argument("--duration", type=float)
    e = sub.add_parser("exec")
    e.add_argument("instance")
    e.add_argument("command")
    lk = sub.add_parser("look")
    lk.add_argument("instance")
    lk.add_argument("--out")
    lk.add_argument("--width", type=int, default=480)
    ns = ap.parse_args(argv)
    try:
        instances = discover(_ports(ns.ports))
        if ns.cmd == "list":
            for i in instances:
                print(f"{i.name:8} {i.role:7} port {i.port} pid {i.pid}")
            if not instances:
                print("no test games running")
            return 0
        inst = resolve(ns.instance, instances)
        if ns.cmd == "state":
            print(json.dumps(state(inst), indent=2))
        elif ns.cmd == "combat":
            print(json.dumps(combat(inst), indent=2))
        elif ns.cmd == "situation":
            print(situation_text(inst))
        elif ns.cmd == "log":
            print(log(inst))
        elif ns.cmd == "order":
            j = order(inst, mode=ns.mode, target=ns.target, point=ns.point,
                      fire=False if ns.no_fire else None, skills=False if ns.no_skills else None)
            print(json.dumps(j.get("order", j)))
        elif ns.cmd == "act":
            print(json.dumps(act(inst, ns.action, ns.duration)))
        elif ns.cmd == "exec":
            print(exec_(inst, ns.command))
        elif ns.cmd == "look":
            data = look(inst, ns.width)
            if data is None:
                print(f"{inst.name}: no window to capture")
                return 1
            out = Path(ns.out or f"{inst.name}.png")
            out.write_bytes(data)
            print(f"saved {out} ({len(data)} bytes)")
        return 0
    except PlayError as err:
        print(f"error: {err}")
        return 1


if __name__ == "__main__":
    sys.exit(main())

import re

from .outcome import Outcome
from .scenario import Scenario

SYSTEM = ("ntdll", "kernelbase", "kernel32", "ucrtbase", "vcruntime140", "msvcp140",
          "user32", "win32u", "gdi32", "d3d11", "dxgi", "ws2_32", "mswsock", "combase")


def _norm(frame: str) -> str:
    f = frame.strip().lower()
    return "battleborn+" + f[len("serverborn+"):] if f.startswith("serverborn+") else f


def _round_reborn(f: str) -> str:
    mod, _, off = f.partition("+")
    try:
        return f"{mod}+{hex(int(off, 16) & ~0xFF)}"
    except ValueError:
        return f


def pick_frame(frames: str | None) -> str:
    if not frames:
        return "unknown"
    fs = [_norm(f) for f in frames.split("|") if f.strip()]
    user = [f for f in fs if f.split("+")[0] not in SYSTEM]
    if not user:
        return fs[0] if fs else "unknown"
    first = user[0]
    if first.startswith("reborn+"):
        nxt = next((f for f in user[1:] if f.startswith("battleborn+")), None)
        if nxt:
            return f"reborn>{nxt}"
        return _round_reborn(first)
    return first


def make(o: Outcome, scn: Scenario) -> str | None:
    if o.kind in ("pass", "running", "harness_error"):
        return None
    if o.kind == "crash":
        return f"crash:{(o.code or 'unknown').lower()}:{pick_frame(o.frame)}"
    if o.kind == "hang":
        return f"hang:{pick_frame(o.frame)}"
    if o.kind == "exit":
        return f"exit:{o.code}"
    if o.kind == "disconnect":
        return f"disconnect:{o.detail}"
    if o.kind == "desync":
        return f"desync:{scn.expect_map or 'unknown'}"
    if o.kind in ("fell", "invisible", "wronghero", "wrongskin", "nocombat"):
        return f"{o.kind}:{scn.expect_map or 'unknown'}"
    if o.kind == "npcsync":   # code: "<missing|ghost|invisible>:<archetype>" (debugloop/npcsync.py)
        return f"npcsync:{o.code}"
    if o.kind == "timeout":
        return f"timeout:{o.phase}"
    return f"{o.kind}:unknown"


def slug(sig: str) -> str:
    return re.sub(r"[^a-z0-9+.]+", "_", sig.lower()).strip("_")

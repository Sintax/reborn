"""Keeps a play session's windows in the standard layout (screenshot.arrange) for as long as the
session runs, so live sessions look like loop runs. Started detached by `loop play`; exits by itself
when the session ends.

    python -m debugloop.windowkeeper
"""
from __future__ import annotations

import time
from pathlib import Path

from . import config, playsession, screenshot

EVERY_S = 2.0


def keep(state_dir: Path = config.STATE_DIR, every_s: float = EVERY_S, arrange=None,
         alive=None, sleep=time.sleep) -> None:
    arrange = arrange or screenshot.arrange
    alive = alive or playsession.alive
    while True:
        info = alive(state_dir)
        if not info:
            return
        try:
            arrange({n: int(r["pid"]) for n, r in info.get("pids", {}).items()})
        except Exception:
            pass   # window placement is cosmetic
        sleep(every_s)


if __name__ == "__main__":
    keep()

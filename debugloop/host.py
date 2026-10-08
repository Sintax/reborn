"""Host a game for human playtesters: the server, and (unless --no-client) the host's own game.

    python -m debugloop.host                      # The Algorithm, waits for 2 players
    python -m debugloop.host --map Portal_P --players 3
    python -m debugloop.host --no-client          # server only

Friends join with Direct Connect to the host's address (UDP port 7777). Nothing here is
graded or killed: close the windows to stop. Logs go to Documents\\RebornLogs as usual.
"""
from __future__ import annotations

import argparse
import sys

from . import config, deploy, launch

SERVER_NAME = "host-server"
CLIENT_NAME = "host-player"


def server_args(map_name: str, players: int) -> list[str]:
    return launch.base_args("server") + [
        f"-rbservermap={map_name}", f"-rbplayers={players}", f"-rbinstance={SERVER_NAME}"]


def client_args() -> list[str]:
    # A normal game, but with its own instance name so it may run beside the server.
    return ["-nomoviestartup", "-NOSPLASH", f"-rbinstance={CLIENT_NAME}"]


def main(argv: list[str] | None = None, launcher=None) -> int:
    p = argparse.ArgumentParser(prog="python -m debugloop.host", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--map", default="Caverns_P", help="map the server opens (default Caverns_P, The Algorithm)")
    p.add_argument("--players", type=int, default=2, help="players the server waits for (default 2)")
    p.add_argument("--no-client", action="store_true", help="start the server only")
    a = p.parse_args(argv)

    if launcher is None:
        deploy.ensure_serverborn(config.WIN64)
        launcher = launch.RealLauncher()
    s = launcher.start(SERVER_NAME, "server", server_args(a.map, a.players))
    print(f"server started (pid {s.pid}) on {a.map}, waiting for {a.players} players on UDP {config.SERVER_PORT}")
    if not a.no_client:
        c = launcher.start(CLIENT_NAME, "client", client_args())
        print(f"your game started (pid {c.pid}): in game, open Direct Connect and enter 127.0.0.1")
    print("friends: Direct Connect to your public or VPN address")
    return 0


if __name__ == "__main__":
    sys.exit(main())

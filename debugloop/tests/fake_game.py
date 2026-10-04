"""Stand-in for the game in harness tests. Speaks the same /state protocol."""
import json
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


def main():
    a = dict(x.partition("=")[::2] for x in sys.argv[1:])
    name = a.get("-rbinstance", "game")
    port = int(a["-rbdebugport"])
    run_dir = Path(a["-rbrundir"])
    mode = a.get("--mode", "pass")
    role = a.get("--role", "solo")
    start = time.time()
    (run_dir / f"{name}.log").write_text(f"[GAME] fake {role} {mode}\n")

    def snapshot():
        if role == "server":
            return {"ticks": 5, "listening": True, "map": "Dojo_P", "connections": 9,
                    "player_locations": {}}
        return {"ticks": 5, "has_pawn": True, "connected": True, "map": "Dojo_P",
                "pawn_location": [0, 0, 0], "autopilot": "playing"}

    class H(BaseHTTPRequestHandler):
        def do_GET(self):
            if mode == "hang" and time.time() - start > 2:
                self.send_response(503); self.end_headers()
                self.wfile.write(b'{"error":"game_thread_unresponsive"}')
                return
            body = json.dumps(snapshot()).encode()
            self.send_response(200); self.end_headers(); self.wfile.write(body)

        def log_message(self, *_):
            pass

    srv = ThreadingHTTPServer(("127.0.0.1", port), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    while True:
        el = time.time() - start
        if mode == "crash" and el > 2:
            (run_dir / f"{name}.1.crash.json").write_text(json.dumps(
                {"code": "0xC0000005", "frames": ["battleborn+0x1234"], "first_chance": False,
                 "dump": f"{name}.1.dmp"}))
            (run_dir / f"{name}.1.dmp").write_bytes(b"MDMP")
            os._exit(3)
        if mode == "exit" and el > 2:
            os._exit(0)
        time.sleep(0.1)


if __name__ == "__main__":
    main()

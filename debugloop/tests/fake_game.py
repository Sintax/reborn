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

    who = {"instance": name, "role": role, "pid": os.getpid()}
    order = {"mode": "hunt", "target": None, "point": None, "fire": True, "skills": True}
    modes = {"hunt", "hold", "goto", "follow", "retreat", "wander"}
    actions = {"jump", "fire_start", "fire_stop", "fire_burst", "altfire", "melee", "skill1", "skill2",
               "ultimate", "use", "sprint_start", "sprint_stop"}

    def combat():
        return {"enabled": True, "mode": order["mode"], "calibrated": True, "aim_on": False, "firing": False,
                "me": {"hero": "Rath", "location": [10, 20, 30], "yaw": 90.0, "pitch": 0.0, "health": 800,
                       "max_health": 1000, "dead": mode == "dead",
                       "skills": [{"slot": 1, "ready": True, "cooldown_s": 0},
                                  {"slot": 2, "ready": False, "cooldown_s": 4.5},
                                  {"slot": 3, "ready": False, "cooldown_s": 60}]},
                "target": "7f0001", "order": order,
                "enemies": [{"id": "7f0001", "hero": "Thrall", "kind": "minion", "distance": 640.0,
                             "bearing": -12.5, "pitch": 2.0, "visible": True, "health": 300, "team": 1,
                             "location": [600, 40, 30]},
                            {"id": "7f0002", "hero": "OscarMike", "kind": "bot", "distance": 2100.0,
                             "bearing": 95.0, "pitch": -1.0, "visible": False, "health": 900, "team": 1,
                             "location": [-100, 2000, 30]}],
                "n_enemies": 2,
                "stats": {"shots": 3, "skills_used": 1, "kills": 0, "deaths": 1, "damage_taken": 200.0,
                          "time_with_target_s": 12.0, "time_firing_s": 4.0}}

    def snapshot():
        if role == "server":
            return {**who, "ticks": 5, "listening": True, "map": "Dojo_P", "connections": 9,
                    "player_locations": {}}
        return {**who, "ticks": 5, "has_pawn": True, "connected": True, "map": "Dojo_P",
                "pawn_location": [0, 0, 0], "autopilot": "playing",
                "combat": {"enabled": True, "stats": combat()["stats"]}}

    class H(BaseHTTPRequestHandler):
        def _json(self, obj, status=200):
            body = json.dumps(obj).encode()
            self.send_response(status); self.send_header("Content-Type", "application/json")
            self.end_headers(); self.wfile.write(body)

        def _text(self, text):
            self.send_response(200); self.end_headers(); self.wfile.write(text.encode())

        def do_GET(self):
            if mode == "hang" and time.time() - start > 2:
                self.send_response(503); self.end_headers()
                self.wfile.write(b'{"error":"game_thread_unresponsive"}')
                return
            if self.path == "/ping":
                return self._text("pong")
            if self.path == "/log":
                return self._text("[GAME] line one\n[COMBAT] target 7f0001\n")
            if self.path == "/combat":
                return self._json(combat())
            self._json(snapshot())

        def do_POST(self):
            raw = self.rfile.read(int(self.headers.get("Content-Length") or 0)).decode("utf-8", "replace")
            if self.path == "/exec":
                return self._text("ok")
            try:
                j = json.loads(raw)
                if not isinstance(j, dict):
                    raise ValueError
            except ValueError:
                return self._json({"error": "bad_json", "status": 400}, 400)
            if self.path == "/order":
                if "mode" in j and j["mode"] not in modes:
                    return self._json({"error": "unknown_mode", "status": 400}, 400)
                for k in ("mode", "target", "point", "fire", "skills"):
                    if k in j:
                        order[k] = j[k]
                return self._json(combat())
            if self.path == "/act":
                if mode == "dead":
                    return self._json({"error": "no_pawn", "status": 409}, 409)
                if j.get("action") not in actions:
                    return self._json({"error": "unknown_action", "status": 400}, 400)
                return self._json({"ok": True, "action": j["action"]})
            self._json({"error": "not_found", "status": 404}, 404)

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

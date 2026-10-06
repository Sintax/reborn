# Combat Autopilot and the "Play" MCP: Design

Date: 6 October 2026
Status: waiting for your review

---

## Part 1: The plain-language version

### What this is

Two things that let the AI (Claude) actually play the test games instead of wandering at random:

1. **Reflexes inside the game.** The test players get a small combat brain: they find enemies, aim at them, shoot, use their abilities, and chase or back off. This runs every frame inside the game, so the shooting is quick enough to matter.
2. **Eyes and hands for Claude.** A small add-on (an "MCP", a plug-in that gives Claude new tools) lets Claude look at any test game window, read what the player can see, and give orders: hunt, hold here, go to this spot, retreat, use the ultimate. Claude is the slow commander; the in-game brain is the fast soldier.

### What you said you want

- Claude should be able to see what it is doing and react in real time.
- The test players should hunt enemy combatants and shoot back, not move randomly.
- Build it, then find the best places to plug it into the current debug-loop plan.

### What I assumed (correct me if wrong)

- "Both games" means the test game windows the debug loop starts (one or two players). Your own copy of the game is never touched: the new tools only work on windows started with the test switches.
- Good-enough aim and straight-line chasing are fine for now. The game's own path-finding (walking around walls) is a later step; the existing "stuck for 5 seconds, jump" trick stays.
- Claude looks at pictures sparingly (they cost a lot of usage) and plays mostly from a short text readout.

### What the player will do on its own

With the combat brain on, a test player:

- keeps a list of enemies it knows about (bots, minions, enemy players), nearest first, with "can I see it" for each;
- turns to face the best target, aims at its chest, and fires while the aim is on;
- walks toward a target that is far away, strafes a bit while shooting, and backs off when its health is low;
- uses its two skills when they are ready and an enemy is in view, and its ultimate when several enemies are near;
- when it sees nothing, wanders as it does today, which is how it finds the next fight.

### What Claude can do with the new tools

| Tool | What it does |
|---|---|
| list games | finds the running test windows (server, player 1, player 2) |
| look | a picture of a player's screen, small by default to save usage |
| situation | one short text block: my health, where I am, my target, the enemies I know about (how far, which way, visible or not), skills ready or cooling down, kills and deaths |
| order | sets the standing behaviour: hunt (default), hold, go to a point, retreat, follow a target, or wander |
| act | one-off actions: jump, fire on/off, skill 1, skill 2, ultimate, melee, use/interact |
| console | sends a raw game command (what the harness can already do) |
| log | the last lines of that window's log |

The same tools work from a command line without Claude, so you can try them yourself.

### Where it plugs into the current plan

1. **The test ladder (steps 2 and 3).** The two-player scenarios turn the combat brain on. The runs then test fighting, dying, respawning, damage shown on both screens, and ability code, which random walking never did. The run report gains a line per player: shots fired, kills, deaths, damage taken. A run where a player never fires or never takes damage in 10 minutes gets a warning in the report, not a failure. Step 1 (one player on the training map) also gets it, so the step-1 regression run tests shooting.
2. **A "play" mode for the loop.** A new command starts a scenario and leaves it running instead of grading and closing it. Claude then uses the tools to drive the players into the situation a bug needs (for example "both players attack the same bot") and watches what happens. The fixer agent's brief says when a live session is available and on which ports.
3. **The bug-fixer agent.** When the loop finds a bug that only shows during a fight, the controller may start a play session and reproduce it on purpose before handing it to the fixer, so the fixer gets exact log lines instead of a 40-minute run to read.
4. **Step 4 (you and your friend).** Not changed. A test player with the combat brain can stand in for the friend when they are not around: you play one window, Claude plays the other.

### Limits to expect

- The in-game aim is decent, not expert. It will miss moving targets at range.
- Chasing is straight-line. A target behind a wall means a stuck-and-jump loop until the wander picks a new direction.
- Claude's decisions arrive every few seconds. It cannot dodge.
- Each picture costs roughly 1,000 to 1,500 tokens at the small size. Ten minutes of looking every 10 seconds is about 100k tokens, so the default is: read the situation text, look only when something seems off.
- The abilities use the game's own slot numbers. The first live run checks which slot is which; some heroes' skills need a target or a ground point and may fail silently from a bare "use skill".

### What could go wrong

- **Aim turns the wrong way or too fast.** The game reads the turn input in its own units. The first live run measures how far one unit of input turns the view per second and the brain is tuned to it. Until then the aim could spin. This is a solo-map test, so nothing is at risk.
- **Enemy detection picks the wrong things.** The game's own "is this my enemy" check is used, so friendly bots and minions should be skipped. Dummies on the training map may or may not count as enemies; the first run tells.
- **The server copy must not get a brain.** The server runs with no graphics and no player; the brain is client-only, as the autopilot already is.
- **Two windows, one keyboard.** Not a problem: orders go through the game's debug port, not the keyboard or mouse.

---

## Part 2: Technical design

### 2.1 Components

| Component | Where | Purpose |
|---|---|---|
| `Combat` (new, C++) | `reborn/Combat.cpp/.hpp` | Per-tick enemy census, target selection, aim, fire, skill use, movement intent. Owned by `Autopilot`. |
| `Autopilot` changes | `reborn/Autopilot.cpp` | `PlayTick` delegates to `Combat::Tick` when combat is on; keeps the wander as the fallback. Orders change its mode. |
| Debug server routes | `reborn/DebugServer.cpp`, `reborn/GameState.cpp` | New `/combat` (GET), `/order` (POST), `/act` (POST). `/state` gains `combat` stats. |
| Launch option | `reborn/LaunchOptions.cpp` | `-rbcombat` (on/off; default off so existing scenarios do not change until their files say so). |
| `debugloop/play.py` (new) | Python | Client library: discovers instances, wraps the HTTP routes, captures a screenshot via `screenshot.capture`. CLI: `python -m debugloop.play <cmd>`. |
| `debugloop/mcp_server.py` (new) | Python | MCP server over stdio (`mcp` package, FastMCP) exposing `play.py` as tools. Registered in `reborn/.mcp.json`. |
| `loop play` (new) | `debugloop/loop.py`, `run.py` | Starts a scenario and leaves it running; `loop stop-play` ends it. |
| Scenario files | `debugloop/scenarios/*.toml` | s1-s3 add `-rbcombat` to client args. |
| Outcome / report | `debugloop/outcome.py`, `run.py` | Combat stats in the run summary and a `warnings` list. |
| `bb-autofix` skill, `bb-fixer` agent | `.claude/skills`, `.claude/agents` | Mention the play session and tools. |

### 2.2 Combat (C++)

Runs only when `Autopilot::Active()` and `LaunchOptions::Get().combat` (or an order turned it on). Called from `Autopilot::PlayTick` after the pawn check, every engine tick on the game thread, so all SDK reads are safe.

**Census** (every 0.25 s, cached between):
walk `WorldInfo->PawnList` via `NextPawn`. Keep a pawn `p` when `LivePawnInWorld(p)` (reuse from `GameState.cpp`, moved to a shared `PawnUtils.hpp`), `p != me`, `p->IsAliveAndWell()`, `GetHealth() > 0`, and `me->IsEnemy(p)` (`APoplarPawn::IsEnemy`, the game's own team test, covers bots, minions, players, and turrets). For each: id (`(uintptr_t)p` as hex), hero/archetype name (`HeroOf`), kind (`player` if `PlayerReplicationInfo && !bBot`, `bot` if PRI with bBot, else `minion`), location, distance, bearing (degrees, `-180..180`, relative to the view yaw), pitch to target, health, visible (`me->FastTrace(aimPoint, eyeLocation)` where `eyeLocation = me->Location + (0,0,me->BaseEyeHeight)` and `aimPoint = p->Location + (0,0,p->BaseEyeHeight * 0.5)`; `FastTrace` returns true when nothing blocks).

**Target** selection: the explicit order target if alive and known; otherwise the visible enemy with the lowest score `distance * (kind == player ? 0.6 : 1.0)`; otherwise none. A target is dropped after 3 s unseen.

**Aim**: desired rotator from `eyeLocation` to the target's `aimPoint` (UE units: 65536 per turn; `atan2` in radians × 10430.38). Error = wrap(desired − `pc->Rotation`). Input: `aTurn = clamp(kYawGain * errYawDeg, −1, 1)`, `aLookUp = clamp(kPitchGain * errPitchDeg, −1, 1)`, written in `Autopilot::WriteAxes` after the engine tick (the same path that already makes `aTurn` work). `kYawGain` starts at `1/30` (full input at 30° error); the first live run measures degrees per second at full input and the gains are set from that. Aim is "on" when both errors are under 4°.

**Fire**: `Exec(L"StartFire")` while aim is on and the target is visible and under `kFireRange` (3000 units, about 30 m); `StopFire` otherwise. Which command is "melee" for a melee hero (`StartFire` or `StartAltFire`) is unknown; both are exposed through `act` and the live check decides.

**Skills**: `pc->StartActionSkillBySlot(slot)` with `ASS_SlotOne`/`ASS_SlotTwo` when `GetActionSkillSlotCooldownTimeRemaining(slot) <= 0`, a target is visible, and at least 2 s since the last skill. `ASS_SlotThree` (the ultimate; confirmed on the first live run) when 2+ enemies are within 1500 units. Skills can be disabled by order (`skills: false`) for runs that test only shooting.

**Movement** by mode:

| Mode | Movement | Fire |
|---|---|---|
| `hunt` (default when combat on) | target: approach to `kEngageRange` (1200), strafe sign flips every 1.5–3 s while in range; no target: existing wander | yes |
| `hold` | stand, turn to targets | yes |
| `goto` point | walk to the point (same yaw-steer as hunt, aim at the point when no target), stop within 150 units, then `hold` | yes |
| `follow` target | keep 600–1200 units from the target | yes |
| `retreat` | back away from the nearest enemy at full reverse (`aBaseY = −1`) for 5 s, then `hold` | yes |
| `wander` | today's random walk | today's 1-in-3-s burst |

Auto-retreat: in `hunt`, when health < 25 % of max and an enemy is within 800 units, switch to `retreat` for 5 s, then back to `hunt`. `kFireRange`, `kEngageRange`, gains, and the retreat threshold live in one `Tuning` struct, overridable per key through `/order` (`tuning: {...}`) so live tuning needs no rebuild.

Stuck handling stays as it is (5 s with < 100 units moved → jump, turn for 1 s).

**Stats** (per process, reset on respawn): `shots` (StartFire calls), `skills_used`, `kills`, `deaths` (from `PlayerReplicationInfo->Kills/Deaths` when the PRI is present), `damage_taken` (health drops summed), `time_with_target_s`, `time_firing_s`.

### 2.3 Debug server routes

All on the existing `127.0.0.1:<port>` server; game-thread work goes through `RunOnGameThread` as today.

- `GET /combat` → JSON: `{ "enabled", "mode", "me": {"hero","location","yaw","pitch","health","max_health","dead","skills":[{"slot","ready","cooldown_s"}]}, "target": id|null, "enemies": [ {id, hero, kind, distance, bearing, pitch, visible, health, location} ... up to 12, nearest first ], "stats": {...}, "order": {...the last order...} }`. Reads only cached census data, so it is cheap.
- `POST /order` body JSON: `{ "mode": "hunt|hold|goto|follow|retreat|wander", "target": id, "point": [x,y,z], "fire": bool, "skills": bool, "tuning": {k: v} }`. All fields optional; missing ones keep their value. Also `"enabled": bool` to turn combat on/off at runtime. Returns the resulting order. Unknown mode → 400.
- `POST /act` body JSON: `{ "action": "jump|fire_start|fire_stop|fire_burst|altfire|skill1|skill2|ultimate|use|sprint_start|sprint_stop|respawn_tap", "duration_s": float }`. Returns `{"ok":true}` or `{"error":"..."}` (no pawn, unknown action). `fire_burst` fires for `duration_s` (default 0.5) through the tick timer.
- `/state` gains `"combat": {"enabled","mode","target","n_enemies","n_visible","stats":{...}}`; the server's `/state` unchanged.

### 2.4 Python client and CLI (`debugloop/play.py`)

- `discover(ports=range(18080, 18090))` → list of `Instance(name, role, port, pid)` from `/ping` + `/state` (0.3 s timeout each). Also reads `debugloop/runs/<active>/active.json` when present for names.
- `state(inst)`, `combat(inst)`, `order(inst, **kw)`, `act(inst, action, duration_s=None)`, `exec_(inst, cmd)`, `log(inst, n=40)`.
- `look(inst, max_width=480)` → PNG bytes via `screenshot.capture(pid, tmp)` then Pillow resize. Returns `None` when the window is not found.
- `situation_text(inst)` → the compact text block used by the MCP tool (~15 lines).
- CLI: `python -m debugloop.play list | state <name> | combat <name> | situation <name> | order <name> <mode> [--target id] [--point x y z] [--no-fire] [--no-skills] | act <name> <action> | look <name> [--out file] | log <name>`. Instance may be a name (`c1`) or a port.
- Uses `urllib` like `run.py` (no new dependency).

### 2.5 MCP server (`debugloop/mcp_server.py`)

- `mcp` package (`pip install mcp`), FastMCP, stdio transport. Tools mirror the CLI: `bb_list`, `bb_situation(instance)`, `bb_combat(instance)` (raw JSON), `bb_look(instance, max_width=480)` (returns an MCP image), `bb_order(instance, mode, target=None, point=None, fire=True, skills=True)`, `bb_act(instance, action, duration_s=None)`, `bb_exec(instance, command)`, `bb_log(instance, lines=40)`, `bb_state(instance)`.
- Registered in `reborn/.mcp.json`: `{"mcpServers": {"battleborn-play": {"command": "python", "args": ["-m", "debugloop.mcp_server"], "cwd": "<repo>"}}}` so it loads when Claude Code starts from `reborn/`. Needs one approval from you the first time.
- Only ever talks to `127.0.0.1`. No tool can start or stop a game; that stays with `loop play` / `loop stop-play` so the harness remains the only thing that owns game processes.

### 2.6 Loop integration

- `python -m debugloop.loop play <scenario>`: builds/deploys like `next`, launches the scenario, waits for the server to listen and clients to reach `playing` (or the startup limit), writes `debugloop/state/play.json` (run dir, ports, pids), prints the instance table, and exits 0 leaving the processes running. No grading and no periodic screenshots in play mode; the `look` tool covers it. `python -m debugloop.loop stop-play` kills those processes and removes `play.json`. `next`/`verify` refuse (exit 2, "a play session is running") while `play.json` exists and its pids are alive, which also keeps the existing "foreign game process" guard honest.
- Scenario files: `s1-dojo-1client*.toml`, `s2-algorithm-2clients*.toml`, `s3-meltdown-2clients-bots*.toml` add `"-rbcombat"` to each client's args. s0 solo stays random (step 0 only tests loading).
- `outcome.py`: no new failure kinds. `run.py` writes `combat.json` in the run dir (final `/state.combat.stats` per client) and the summary gains a `warnings` list: `"<name>: fired 0 shots in <N> min"`, `"<name>: took no damage in <N> min"` when the run lasted ≥ 10 min of play. `loop next` prints warnings after the outcome line; the brief includes them.
- `bb-autofix` skill: a short "Live reproduction" section: when the open bug's brief mentions combat (damage, skill, death, respawn, weapon) and the attempt count is 0, the controller may run `loop play <scenario>`, use the `battleborn-play` tools for at most 10 minutes to reproduce it, append the exact log lines to `debugloop/state/brief.md` under "Live reproduction", then `loop stop-play`, then dispatch the fixer as usual. `bb-fixer.md`: add the `play.py` CLI to its tools list with the rule "never start or stop the game yourself".

### 2.7 Error handling

- Game side: every `/combat`, `/order`, `/act` handler returns JSON errors (`no_pawn`, `not_client`, `bad_json`, `unknown_mode`, `unknown_action`) with status 400/409 and never throws across the HTTP boundary. Census code checks `Gone()` on every pointer before use; a pawn that disappears mid-tick is skipped on the next census.
- Python side: `PlayError` with a plain message; the CLI prints it and exits 1; the MCP tool returns it as text, never raises, so Claude sees "player c2 is dead; no pawn" instead of a traceback.

### 2.8 Testing

- C++: no unit harness exists for the DLL. Verification is live: (1) solo Dojo with `-rbcombat`, read `/combat` and watch the aim converge (log `[COMBAT] aim err yaw/pitch` once per second for the first run only, behind a tuning flag); (2) s3 smoke with two combat clients; check `shots > 0`, `damage_taken > 0`, no new crash signature; (3) the ladder's s1 smoke run still passes.
- Python: `pytest debugloop/tests`: `test_play.py` against `fake_game.py` extended with the three routes (discovery, order round-trip, act errors, situation text format, look returns None without a window); `test_mcp_server.py` calls the tool functions directly (no transport); `test_loop.py` gains `play`/`stop-play` cases (play.json lifecycle, `next` refuses while a play session is alive); `test_run.py` gains the warnings cases; `test_scenario.py` checks the edited scenario files still parse.

### 2.9 Out of scope (later)

- Pathfinding with the game's navigation mesh (the risky piece; a straight-line chase ships first).
- Reading the screen to find enemies (the census asks the engine instead).
- Driving the server copy or your own game window.
- Hero-specific skill logic (ground-targeted skills, buffs).

### 2.10 Order of work

1. Game side: `Combat` census + `/combat` (read-only) → live check on Dojo.
2. Aim + fire + modes + `/order` + `/act` → live tuning run.
3. `play.py` + CLI + tests.
4. MCP server + `.mcp.json` → try from this session.
5. `loop play/stop-play`, scenario flags, warnings, skill/agent text.
6. One s3 smoke with combat on; commit; hand back to the loop.

Branch: `agent/autofix` (where the loop expects to find the mod). Note: the tree currently holds an uncommitted skin fix in `reborn/Hooks.cpp` (hero-token skin substitution, ~80 lines) from the paused session; this work does not touch those lines, and the loop's own `verify` decides its fate.

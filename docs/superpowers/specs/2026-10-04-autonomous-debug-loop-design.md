# Self-Running Bug Fixer for Battleborn Reborn: Design

Date: 4 October 2026
Status: waiting for your review

---

## Part 1: The plain-language version

### What this is

A system that keeps playing Battleborn by itself, notices when it breaks, works out why, fixes the mod's code, and checks the fix. It repeats this until the game can be played reliably by two people, with as little help from you as possible.

### What you said you want

- The AI does the work on its own. Human players are hard to find and need rest.
- Real human play comes later, once the AI has cleared the obvious problems.
- The top-tier model (Fable 5.1) does the hard thinking; cheaper models do routine chores.
- The AI may install free debugging tools when it needs them.
- You review this document, and then the build plan, before anything is built.

### What I assumed (correct me if wrong)

- Everything runs on this laptop (ROG Strix, 16 GB memory), where the game is installed.
- The AI saves its work on a separate work branch. Nothing is sent to GitHub unless you ask.
- The first target is co-op story missions with two players, then PvP. Ten-player matches and public servers come in a later project.

### How one round of the loop works

1. **Run a test match.** A script starts the server and one or two copies of the game. The game copies connect by themselves and "play" with a simple autopilot: walk, turn, jump, shoot. The game's own bots fill the empty slots.
2. **Watch it.** Each copy reports "still alive, here's what I'm doing" every few seconds. A crash, freeze, disconnect, or running past the time limit counts as a failure.
3. **Collect evidence.** On a crash the game saves a snapshot of its memory at that moment (a "crash dump") and its last log lines. Everything for that match goes into one folder.
4. **Group it.** Crashes at the same spot in the code count as one bug. Each bug gets a notes page listing what happened, what was tried, and what didn't work, so the AI never repeats a failed idea.
5. **Diagnose and fix.** Fable 5.1 reads the evidence and the code, works out the cause, and makes the smallest fix that should work.
6. **Prove it.** Rebuild, then re-run the failing test until it passes several times in a row. Also re-run quick versions of the earlier tests to catch new breakage. Pass: the fix is saved. Fail: the fix is undone and the attempt is written down.
7. **Stuck rule.** After 5 failed attempts on one bug, the AI parks it, writes a question for you, and moves on to the next problem.

### The goal, as a ladder

Each step must pass reliably before the AI moves to the next one.

| Step | What has to happen | "Reliably" means |
|---|---|---|
| 0 | The modded game starts and loads a solo mission | 3 starts in a row |
| 1 | The server runs and one autopilot player plays 15 minutes on the training map | 3 runs in a row, no crash |
| 2 | Two autopilot players play a co-op story mission for 30 minutes: fighting, dying, respawning. No crash, freeze, disconnect, and the server and players must agree on where everyone is. | 2 runs in a row |
| 3 | A PvP match runs to the end with two autopilot players plus bots | 2 runs in a row |
| 4 | You and your friend play for real, including finishing a story mission | Your call |

Step 4 is the only one that needs people. The AI tells you when step 3 passes.

*Changed 4 Oct while planning:* the autopilot wanders randomly, so it can't complete story objectives. Step 2 now tests 30 minutes of real play, and finishing a mission moves to step 4. PvP matches end on their own because bots play the objective, so step 3 is unchanged.

### What you'll see while it runs

- A short **progress log**: which step it's on, which bugs it found, which it fixed.
- A **question list**: anything it got stuck on, written in plain language.
- A **notification on your phone** when a step passes or when it needs you.

### Safety rails

- Code changes go only on the work branch. Your main version is never touched.
- In the game folder it only adds or replaces the mod's own files, and keeps a backup of anything it replaces. It never changes the game's own data files.
- It won't start a test if the laptop has less than about 2 GB of free memory. It shuts down every game copy it started when a test ends.
- It never changes Windows system settings.
- It only installs free, well-known debugging tools from their official sources.

### What could go wrong

- **The game might not start at all with the mod on this machine.** Step 0 exists to find this out first. If the cause is outside the mod's code, for example the fake-Steam layer, the AI will try the known fixes and then ask you.
- **Two game copies plus a server might not fit in 16 GB.** The AI measures this early. Fallbacks: low graphics settings, small windows, and running step 2 with one autopilot player plus bots.
- **Some bugs may be too deep for a quick fix**, like the garbage-collection crashes the original author fought (the engine's routine cleanup of unused memory). The stuck rule keeps those from stalling everything else.
- **Two copies of the game on one PC may look like the same Steam user.** The plan's first task tests this. The fallback is a second copy of just the small program folder with its own fake-Steam identity.
- **Windows Defender quarantines the fake-Steam launcher.** Seen on 4 October 2026 (Task 14): the first live run read `steamclient_loader_x64.exe` to copy it into the per-player folder, and Defender flagged it ("Trojan:Win32/Wacatac.H!ml", a machine-learning guess that commonly hits this tool) and removed it from the game folder. Nothing can launch until you restore it, or allow it, in Windows Security. The AI is not allowed to change security settings, so this one is yours. The runner now stops with a plain message naming the file instead of a cryptic "Invalid argument". Each per-player folder gets its own copy of the launcher, so a folder exclusion for the game folder (or for `rb_ids`) is the durable fix. (You restored it and added the exclusion the same day.)
- **Found in the first live runs (Task 14, 4 October 2026):**
  - The mod's save picker waited for a click forever, so the autopilot never reached the menu. With the autopilot on, the picker is now skipped: it always uses its own save named "autopilot" (everything unlocked), creating it in the game's own SaveData folder if missing. Your own saves are never used. That save will show up in the picker when you play by hand.
  - The game answers "busy" while it loads, before its main loop has run once. The runner used to call that a freeze after 15 seconds. Now it waits; the 4-minute startup limit covers a game that never finishes loading.
  - The game has its own crash handler. It catches a crash, writes "Critical error" and its own dump to `Documents\My Games\Battleborn\PoplarGame\Logs`, and then sits there without closing. So the mod's "final" crash report never appears. The runner now counts a crash on the game's main thread as a crash once that thread stops answering.
  - The runner gave up on a frozen game at the same moment the mod's freeze watchdog was writing its report, so the report was lost. The runner now waits for the watchdog's limit plus 10 seconds before calling it an unexplained freeze.
  - The crash analyzer's debugger command mangled the symbol path, so Windows' own function names were missing. Fixed.
  - A crash that the game's own handler catches before the game's main loop has run once is graded `timeout:startup` (after 4 minutes), not `crash`. The "game stopped answering" rule only starts counting after the first tick.
  - The autopilot's character spawns but does not move or jump (spike S1; fixed in Task 15, see below).
  - Mod dumps are about 0.9 MB, not over 1 MB as first guessed. They still hold everything the analyzer needs.
- **Found in the multiplayer bring-up (Task 15, 4 October 2026):**
  - **The game refuses a second copy.** A client started while the server ran showed "An existing instance of the game is already running" and waited for a click. The game checks a lock named `Shipping_Poplar_Client_Mutex`. Copies started by the runner (they carry `-rbinstance=NAME`) now each use their own lock name, so a server and two clients run side by side. Normal launches are unchanged.
  - **Two identities work (S2).** Each copy gets its own fake-Steam folder and Steam ID; the game made a separate SaveData folder for each. Values with spaces arrive intact (the log shows `character="Oscar Mike"`), and the fake-Steam launcher accepts the full path to the game in its settings.
  - **Clients do not get into the match.** The client sends its "hello" packets to port 7777, but the server never answers and never logs a new player. The client drops back to the menu and retries every 90 seconds; the run ends as `timeout:startup`. Without the server's `-nullrhi` it is the same. This matches the known state of the mod ("two-player worked briefly in mid-2025, then broke"), so it is left as the loop's first real bug, not fixed during bring-up. Until it is fixed, the "same player on server and client" check and multiplayer movement cannot be seen live.
  - **Memory (S3).** Peak per copy: server 951 MB (Caverns, no rendering), clients 1,156 and 1,072 MB at 640x360; 3.2 GB together, far below the 13 GB limit. But your other open apps leave only about 2.2 GB free before any game starts, so free memory falls to about 0.4 to 0.5 GB during a three-copy run even with the small-window fallback (now always on in multiplayer: server `-nullrhi`, clients 640x360). Windows copes by paging; nothing failed because of it.
  - **The autopilot did not move (S1), now fixed.** Battleborn runs its per-frame player update directly, never through the hook the autopilot listened on, so the steering code never ran. The steering is now written at the end of each game tick, and the next frame uses it: in a solo run the character walked up to 4,700 units from the start. Multiplayer movement waits on the join bug above.
  - **The autopilot's save follows the first SaveData folder.** The mod picks the first folder it finds, and each new test identity adds a folder, so a new player may create a fresh "autopilot" save in a different folder. Harmless, but it means more than one "autopilot" save can exist.
  - **Code map (S4).** Ghidra's export finished in about 25 minutes: 169,979 functions, and all 15 hook addresses the mod uses sit at the start of a function. The copy protection did not spoil the analysis, so crash reports can name the game function a crash happened in.

---

## Part 2: Technical details

### Where things live

| Item | Location |
|---|---|
| Mod source (C++) | `reborn/reborn/` (fork `Sintax/reborn`) |
| New harness code | `reborn/debugloop/` (Python 3.14) |
| Bug notes, progress log, question list | `reborn/debugloop/bugs/`, `reborn/debugloop/PROGRESS.md`, `reborn/debugloop/QUESTIONS.md` |
| Loop instructions (Claude Code skill) | `reborn/.claude/skills/bb-autofix/SKILL.md` |
| Run artifacts (dumps, logs; large, not in git) | `Battleborn-Server/runs/<run-id>/` |
| Backups of replaced game files | `Battleborn-Server/backups/<timestamp>/` |
| Game install | `D:\SteamLibrary\steamapps\common\Battleborn\Binaries\Win64` |
| Work branch | `agent/autofix`, created from `local-dev` |

The two uncommitted edits on `local-dev` become the first commit on `agent/autofix`, because both are needed for local testing. One points the lobby address at `localhost:5000`; the other fixes the inverted loop condition in `MatchLaunchService.cs`.

### Component 1: In-game instrumentation (C++, inside `reborn.dll`)

New files, each with one job:

- **`LaunchOptions.cpp/.hpp`** parses the command line once at startup. Today map and player count are hard-coded in `ServerSettings.cpp`. Defaults keep today's behaviour when a flag is absent.
  - `-rbinstance=<name>`: names the instance, e.g. `server`, `c1`, `c2`; used in log and dump file names.
  - `-rbmap=<map>`, `-rbplayers=<n>`: server map and number of players to start.
  - `-rbconnect=<host:port>`: the client connects automatically once the main menu is up, using the same code path as the overlay's Direct Connect.
  - `-rbautoplay`: turns on the autopilot.
  - `-rbdebugport=<port>`: port for the status endpoint (default `0`, meaning off).
  - `-rbrundir=<path>`: where to write logs and dumps.
  - `-rbtestcrash=<seconds>` / `-rbtesthang=<seconds>`: deliberately crash or freeze after N seconds. Used only to self-test the crash and hang capture.
  - `-rbsolomap=<map>`: client starts that map in solo mode, used for step 0.
  - `-rbcharacter=<index>`: which character the autopilot locks in.
  - `-rbseed=<n>`: seed for the wandering pattern.
  - `-rbhangsecs=<n>`: freeze threshold.
- **`Diagnostics.cpp/.hpp`**:
  - Every existing `printf` also goes to `<rundir>/<instance>.log`, with timestamps and a 500-line in-memory ring buffer.
  - An unhandled-exception filter, plus a vectored handler as backup, writes `<instance>.dmp` with `MiniDumpWriteDump` (MiniDumpWithIndirectlyReferencedMemory, plus thread info). It also writes `<instance>.crash.json` with the exception code, the faulting thread's stack as `module+offset` frames, and the ring buffer.
  - A watchdog thread detects a frozen game: no engine tick for `-rbhangsecs` seconds (default 60, because map loads block the game thread). It writes a dump and `hang.json` without killing the process.
  - Fatal exceptions are caught first-chance by a vectored handler: the engine catches crashes itself, so an unhandled-exception filter alone would never fire. Reports carry `first_chance: true`. The runner counts them only if the process then dies or hangs, so exceptions that the anti-tamper layer handles internally are ignored. At most 3 dumps per process.
  - With no `-rbrundir`, logs and dumps go to `Documents\RebornLogs\`, so human sessions in step 4 also leave evidence.
- **`DebugServer.cpp/.hpp`**: uses the cpp-httplib copy already in the repo; listens only on `127.0.0.1`.
  - `GET /state` returns JSON: instance, server/client role, uptime, tick count, last-tick time, current map, connection count with per-connection state, replicated actor count, process memory, and player pawn location and health when there is one.
  - `POST /exec` runs a console command and returns. This lets the runner and the AI poke a live game.
  - Requests are queued and handled on the game thread inside the existing `GameEngineTickHook`. Engine calls never run off-thread.
- **`Autopilot.cpp/.hpp`** (client only, when `-rbautoplay` is set):
  - Every tick, writes `UPlayerInput` axis fields (`aBaseY`, `aStrafe`, `aTurn`; offsets in `BB/SDK_HEADERS/Engine_classes.hpp`) to walk and turn in a wandering pattern.
  - Calls `StartFire`/`StopFire` and `Jump` through `ProcessEvent` on a timer.
  - Respawns when dead and re-sends ready-up where the mode needs it.
  - Deliberately dumb: its job is to stress replication, not to win.
  - The wandering pattern is seeded from the run id, so a run can be replayed.

### Component 2: Test runner (Python, `debugloop/`)

- **`scenarios/*.toml`**: one file per test. Fields: which processes to start, flags for each, time limit, pass condition (`survive`, `mission_complete`, `match_end`, or `main_menu_loaded`), and the ladder step it belongs to. Every step gets a short smoke version (≤5 minutes) for regression checks.
- **`run.py <scenario>`**:
  1. Checks free memory (refuses below 2 GB) and that no game process is already running.
  2. Creates `runs/<id>/`.
  3. Launches the processes. Clients go through the Steam emulator loader; the exact method is settled by Spike S1 below.
  4. Polls each `/state` every 2 seconds and records a timeline.
  5. Detects the outcome: process exit, `.dmp` present, `hang.json`, a client's connection dropping to zero, or time limit hit.
  6. Kills everything it started, and only that. Started process ids are recorded in `runs/active.json`. A game the runner did not start, e.g. you playing, makes it refuse with a harness error instead of killing anything.
  7. Desync check: each client's own player position must be within 1500 units of some player position on the server. Breaking that for 10 seconds straight counts as a `desync` failure.
  8. Retention: passing runs keep logs but delete dumps, and at most the 3 newest dumps per bug are kept.
  9. Writes `result.json`: outcome, signature, artifact paths, peak memory per process.
  10. Exit code: 0 pass, 1 fail, 2 harness error. A harness error is never counted as a game bug.
- **Signatures**:
  - Crash: `crash:<exception code>:<first frame inside Battleborn.exe or reborn.dll as module+offset>`.
  - Hang: `hang:<module+offset of game thread top frame>`.
  - Disconnect: `disconnect:<last NETWORKING log line category>`.
  - Timeout: `timeout:<last state>`.
- **`analyze.py <run-id>`**: quick automatic triage with the Python `minidump` package: stack, registers, and nearby memory. When available it also runs `cdb -z <dmp> -c "!analyze -v; kb; q"` (WinDbg's command-line debugger). It maps offsets to known names using the hook table in `Init.cpp`, `BB/NameDump.txt`, and the Ghidra project. Writes `triage.md` into the run folder.
- **`deploy.py`**: copies the freshly built `reborn.dll` and `dxgi.dll` into the game folder. It creates `Serverborn.exe` if missing, after backing up any file it overwrites, then checks the copied file's hash.
- **`ledger.py`**: creates or updates `bugs/<signature-slug>.md` with status (`open`, `fixing`, `fixed`, `parked`), first and last seen, run ids, attempts (hypothesis, change, result), and the commit that fixed it.
- Unit tests in `debugloop/tests/` (pytest) for signature bucketing, the outcome detector (fed recorded timelines), the ledger, and the scenario parser. They use fake processes, so they don't need the game.

### Component 3: The loop (Claude Code skill `bb-autofix`, run under `/loop`)

Main session: Opus 5.5. It keeps the state machine and does no deep reasoning itself.

1. Read `debugloop/state.json`: current step, current bug, attempt count. If missing, start at step 0.
2. Build: MSBuild via VS 2022 Build Tools, `reborn.sln` Release x64. A build failure goes back to whoever made the change.
3. Deploy, then run the current step's scenario.
4. **Pass:** count consecutive passes. When the step's threshold is reached, run every lower step's smoke test, move up a step, append to `PROGRESS.md`, and send a phone notification.
5. **Fail:**
   1. Run `analyze.py` and update the ledger.
   2. Dispatch a **Fable 5.1 subagent** with the bug page, `triage.md`, run artifacts, and source paths. Its brief: follow systematic debugging, state one hypothesis, change only `reborn/reborn/*` (and `gamecontroller/` if the cause is there), build cleanly, and return the hypothesis and diff summary. It may use Ghidra and x64dbg through their MCP servers, and launch the game through `run.py` to gather evidence.
   3. Re-run the failing scenario to the step's threshold, plus lower-step smoke tests. If they all pass, commit on `agent/autofix` with the message `fix(<bug-slug>): <hypothesis>` and mark the bug fixed. Otherwise `git restore` the change, record the attempt, and increment the count.
   4. After 5 failed attempts, mark the bug `parked`, add an entry to `QUESTIONS.md`, notify you, and switch to the next open bug. If nothing else is open, try a different scenario at the same step, e.g. another map.
6. Harness errors (exit code 2) go to a **Sonnet 5.5 subagent** that fixes `debugloop/` code only. Three harness errors in a row: stop the loop and notify you.
7. Keep-awake is requested while the loop runs. All state is on disk, so a fresh session resumes where the last one stopped.

### Tools the AI may install (approved by you on 4 October 2026)

| Tool | Purpose | Source |
|---|---|---|
| `minidump` Python package | Read crash dumps without a debugger | PyPI |
| WinDbg / `cdb` | Full crash analysis | Microsoft (winget `Microsoft.WinDbg`) |
| Java 21 + Ghidra + GhidraMCP | Decompile game code at crash addresses; one saved project for `Battleborn.exe` | Adoptium, NSA GitHub releases |
| x64dbg + an MCP plugin | Live debugging when a dump isn't enough | x64dbg GitHub releases, `duty1g/x64dbg-mcp-server` |

### Spikes: questions answered in the first tasks of the plan

- **S1 Launch:** can Battleborn start through `steamclient_loader_x64.exe` with `-seekfreepackagemaps -seekfreeloadingpcconsole` and extra flags in `ExeCommandLine`? Can a second loader instance use a different ini (for `Serverborn.exe`)? If not, does launching the exe directly work with the emulator DLLs in place?
- **S2 Two identities:** do two client copies on one PC collide on Steam ID? Fallback: a sibling folder `Binaries/Win64_c2/` with copies of the exe, mod and emulator DLLs, and its own `steam_settings` identity. The game's relative paths (`..\..\PoplarGame`) still resolve from there.
- **S3 Memory:** peak memory of server, one client, and two clients at low settings. This decides whether step 2 uses two autopilot clients or one client plus bots.
- **S4 Crash capture:** `-rbtestcrash` produces a readable dump and `crash.json`, and the runner classifies it correctly.

### Out of scope for this project

- Ten-player matches, public servers, an installer for other players, the lobby service (`gamecontroller`) beyond what local testing needs, scoreboards, cosmetics.
- Pushing to GitHub or contacting the Reborn Discord. The AI may draft a message, but you decide whether to send it.

### Success criteria for this project

- Steps 0–3 of the ladder pass at their thresholds with the autopilot.
- Every fix is a separate commit on `agent/autofix`, linked from a bug page with its evidence.
- The harness's own unit tests pass.
- `PROGRESS.md` explains in plain language what was broken and what changed, so you can review the work without reading code.

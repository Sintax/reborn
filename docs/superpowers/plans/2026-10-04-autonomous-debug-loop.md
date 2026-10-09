# Autonomous Debug Loop Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A loop that launches Battleborn (server + AI-driven clients), detects crashes, freezes, disconnects and desyncs, records each as a bug, has an AI fix it, rebuilds, re-tests, and climbs the ladder (step 0 → 4) with no human in the loop.

**Architecture:** Inside the game, reborn.dll gains four small modules: launch flags, crash/freeze capture, a localhost debug web server, and an autopilot that plays. Outside the game, a Python package (`debugloop/`) builds, deploys, launches scenarios, grades them, keeps a bug ledger and drives the fix loop. A Claude Code skill (`bb-autofix`) runs that package under `/loop` and hands each bug to a Fable 5.1 subagent.

**Tech Stack:** C++20 / MSBuild (existing reborn.vcxproj), safetyhook, cpp-httplib + nlohmann json (both already in `reborn/`), Python 3.14 + pytest + psutil + minidump, WinDbg `cdb.exe`, Ghidra 12.1.4 headless, gbe_fork ColdClientLoader.

**Spec:** `docs/superpowers/specs/2026-10-04-autonomous-debug-loop-design.md`

All paths below are relative to the repo root `C:\Users\djsin\Documents\GitHub\Battleborn-Server\reborn` unless absolute.

## Global Constraints

- Work only on local branch `agent/autofix`. Never push. Never commit to `main`.
- Never change Windows system settings. Never suggest port forwarding.
- Before overwriting any file in the game folder, back it up (`deploy.py` does this; never copy by hand).
- The runner kills only processes it started (recorded in `runs/active.json`). If a game process it did not start is running, it refuses to run (exit code 2).
- The fix agent may edit only `reborn/` and `gamecontroller/`, never `reborn/BB/` (the generated SDK).
- Game path default: `D:\SteamLibrary\steamapps\common\Battleborn`; override with env `BB_GAME_DIR`.
- Game logs default dir: `%USERPROFILE%\Documents\RebornLogs`.
- Freeze watchdog default: 60 s without a game tick (`-rbhangsecs=N` overrides).
- At most 5 crash reports per game process; identical crash addresses are reported once.
- Desync rule: server and client disagree on a player's position by more than 1500 units for 10 s (5 samples at 2 s).
- Startup timeout: 240 s from launch to "playing".
- Fix attempts per bug: 5, then give up and move on. 3 harness errors in a row: stop and notify.
- Retention: delete dumps of passing runs; keep the 3 newest dumps per bug.
- Debug server binds 127.0.0.1 only. Ports: first instance 18080, then +1. Game server port 7777.
- Free disk below 2048 MB: refuse to run.
- Ladder: step 0 solo mission loads, 3 passes in a row. Step 1 server + 1 autopilot, 15 min on training map, 3 in a row. Step 2 two autopilots play a story mission 30 min with no crash, freeze, disconnect or desync, 2 in a row. Step 3 PvP match with bots runs to the end, 2 in a row. Step 4 (added 2026-10-09) two players with story enemies and Meltdown lane minions, pressing their skills, with the server and both clients agreeing on every computer-controlled character (`check_npcs`, debugloop/npcsync.py). Step 5 humans (out of scope for automation).

## Review Focus

1. **A game window left open from a manual session** → runner must refuse (exit 2), not kill it. Test in Task 10 (`test_refuses_when_foreign_game_running`).
2. **Game exits cleanly (code 0) mid-run without "Match ended"** → must grade as `exit:0`, not pass. Test in Task 3.
3. **Debug server unreachable because the game thread is frozen** → `/state` answers 503 `game_thread_unresponsive`; runner treats repeated 503 as a hang even if the watchdog file is missing. Tests in Task 3 (`test_three_503s_is_hang`) and Task 7 (`test_debug_server`).
4. **The same crash at a different address after a rebuild** → signature must use module+offset of the first frame *outside* reborn.dll when reborn.dll offsets shift; ledger matches by signature. Test in Task 3.
5. **Loop state file half-written when the laptop sleeps or power drops** → state saves atomically (write temp, rename); a corrupt file is reported, not silently reset. Test in Task 4.

---

## File Map

Python (`debugloop/`):

| File | Responsibility |
|---|---|
| `debugloop/__init__.py` | empty |
| `debugloop/config.py` | paths, limits, ports |
| `debugloop/build.py` | find MSBuild / vcvars, build reborn.dll and native test exes |
| `debugloop/scenario.py` | load scenario TOML files, ladder order |
| `debugloop/outcome.py` | grade a run from samples + process records |
| `debugloop/signature.py` | turn an outcome into a stable bug signature |
| `debugloop/ledger.py` | bug ledger (`state/ledger.json` + `state/ledger.md`) |
| `debugloop/state.py` | loop state, atomic save |
| `debugloop/deploy.py` | copy reborn.dll into the game folder with backups; create Serverborn.exe |
| `debugloop/launch.py` | start game processes with per-instance Steam identity; read exit codes |
| `debugloop/run.py` | run one scenario end to end; CLI `python -m debugloop.run` |
| `debugloop/analyze.py` | write `triage.md` for a failed run (cdb, Ghidra names, known hooks) |
| `debugloop/loop.py` | loop controller CLI: `next`, `verify`, `giveup`, `status`, `reset-stop` |
| `debugloop/scenarios/*.toml` | ladder scenarios + smoke variants |
| `debugloop/scenarios/selftest/*.toml` | harness self-tests using fake game / crash flags |
| `debugloop/tests/*.py` | pytest |
| `debugloop/tests/fake_game.py` | stand-in game process for tests |
| `debugloop/ghidra/ExportFunctions.java` | Ghidra headless script: function name + address list |

C++ (`reborn/`, added to `reborn/reborn.vcxproj`):

| File | Responsibility |
|---|---|
| `reborn/LaunchOptions.hpp/.cpp` | parse `-rb*` command-line flags |
| `reborn/Diagnostics.hpp/.cpp` | log tee, crash capture (VEH + unhandled filter), minidumps, freeze watchdog |
| `reborn/DebugServer.hpp/.cpp` | 127.0.0.1 HTTP: `/state`, `/exec`, `/log`; work runs on the game thread |
| `reborn/GameState.hpp/.cpp` | build the `/state` JSON snapshot from the SDK |
| `reborn/Autopilot.hpp/.cpp` | menu → join/solo → pick character → play (walk, look, jump, fire) |
| `reborn/tests/test_launch_options.cpp` | native test exe |
| `reborn/tests/test_debug_server.cpp` | native test exe (port 18999) |
| `reborn/tests/crashme.cpp` | native exe: `crash`, `hang`, `spam` modes to test Diagnostics |

Other:

| File | Responsibility |
|---|---|
| `.claude/skills/bb-autofix/SKILL.md` | the loop skill run as `/loop /bb-autofix` |
| `.gitignore` | add `debugloop/runs/`, `debugloop/state/`, `debugloop/ghidra/out/` |

## Task Order

1. Baseline: commit pending edits, Python package + build helper, prove reborn.dll builds
2. Scenarios
3. Grading + bug signatures
4. Ledger + loop state
5. C++ launch flags
6. C++ crash and freeze capture
7. C++ debug server + game state
8. C++ autopilot
9. Wire modules into the mod; deploy
10. Launcher + scenario runner
11. Triage (cdb, Ghidra)
12. Loop controller
13. bb-autofix skill
14. Harness self-test on the real game (crash / freeze flags)
15. Live bring-up: spikes S1–S4, steps 0 and 1 by hand-run of the loop
16. Start the unattended loop

---

### Task 1: Baseline

**Files:**
- Commit: `gamecontroller/Services/MatchLaunchService.cs`, `reborn/Constants.hpp` (user's pending edits)
- Create: `debugloop/__init__.py`, `debugloop/config.py`, `debugloop/build.py`, `debugloop/tests/__init__.py`, `debugloop/tests/test_build.py`, `debugloop/requirements.txt`
- Modify: `.gitignore`

**Interfaces:**
- Produces: `config.REPO, GAME_DIR, WIN64, RUNS_DIR, STATE_DIR, SCENARIOS_DIR, LOG_DIR, MIN_FREE_MB, FIRST_DEBUG_PORT, SERVER_PORT`; `build.find_msbuild() -> Path`, `build.find_vcvars() -> Path`, `build.msbuild_command(config="Release") -> list[str]`, `build.build_mod() -> BuildResult`, `build.build_native(name: str, sources: list[Path], out_dir: Path, extra_flags: list[str] = ()) -> BuildResult`; `BuildResult(ok: bool, output: str, artifact: Path | None)`.

- [ ] **Step 1: Commit the pending edits**

```bash
git checkout agent/autofix
git add gamecontroller/Services/MatchLaunchService.cs reborn/Constants.hpp
git commit -m "fix: GC launch loop honours cancellation; point GC endpoint at localhost:5000"
```

- [ ] **Step 2: Install Python deps**

`debugloop/requirements.txt`:
```
pytest>=8
psutil>=6
minidump>=0.0.24
```
Run: `python -m pip install -r debugloop/requirements.txt`

- [ ] **Step 3: Write `debugloop/config.py`**

```python
import os
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
MOD_PROJECT = REPO / "reborn" / "reborn.vcxproj"
MOD_DLL = REPO / "reborn" / "x64" / "Release" / "reborn.dll"
GAME_DIR = Path(os.environ.get("BB_GAME_DIR", r"D:\SteamLibrary\steamapps\common\Battleborn"))
WIN64 = GAME_DIR / "Binaries" / "Win64"
PKG = REPO / "debugloop"
RUNS_DIR = PKG / "runs"
STATE_DIR = PKG / "state"
SCENARIOS_DIR = PKG / "scenarios"
NATIVE_OUT = PKG / "native"
LOG_DIR = Path(os.environ["USERPROFILE"]) / "Documents" / "RebornLogs"

MIN_FREE_MB = 2048
FIRST_DEBUG_PORT = 18080
SERVER_PORT = 7777
HANG_SECONDS = 60
```

Note: if Step 6 shows MSBuild writes the DLL elsewhere (e.g. `reborn/x64/Release/` at repo root), fix `MOD_DLL` to the real path.

- [ ] **Step 4: Write the failing test `debugloop/tests/test_build.py`**

```python
from pathlib import Path
from debugloop import build


def test_msbuild_command_targets_release_x64():
    cmd = build.msbuild_command("Release")
    assert cmd[0].lower().endswith("msbuild.exe")
    assert "/p:Configuration=Release" in cmd
    assert "/p:Platform=x64" in cmd
    assert cmd[1].endswith("reborn.vcxproj")


def test_find_vcvars_exists():
    assert build.find_vcvars().name == "vcvars64.bat"


def test_build_native_compiles_hello(tmp_path: Path):
    src = tmp_path / "hello.cpp"
    src.write_text('#include <cstdio>\nint main(){std::puts("hi");return 0;}\n')
    res = build.build_native("hello", [src], tmp_path)
    assert res.ok, res.output
    assert res.artifact == tmp_path / "hello.exe" and res.artifact.exists()


def test_build_native_reports_compile_error(tmp_path: Path):
    src = tmp_path / "bad.cpp"
    src.write_text("int main(){ return nope; }\n")
    res = build.build_native("bad", [src], tmp_path)
    assert not res.ok
    assert "nope" in res.output
```

Run: `python -m pytest debugloop/tests/test_build.py -v` → FAIL (`ImportError: cannot import name 'build'`).

- [ ] **Step 5: Write `debugloop/build.py`**

```python
import subprocess
from dataclasses import dataclass
from pathlib import Path

from . import config

VSWHERE = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe")


@dataclass
class BuildResult:
    ok: bool
    output: str
    artifact: Path | None


def _vs_root() -> Path:
    out = subprocess.run(
        [str(VSWHERE), "-latest", "-products", "*", "-requires",
         "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
        capture_output=True, text=True, check=True).stdout.strip()
    if not out:
        raise RuntimeError("Visual Studio with C++ tools not found")
    return Path(out.splitlines()[0])


def find_msbuild() -> Path:
    p = _vs_root() / "MSBuild" / "Current" / "Bin" / "MSBuild.exe"
    if not p.exists():
        raise RuntimeError(f"MSBuild not found at {p}")
    return p


def find_vcvars() -> Path:
    p = _vs_root() / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
    if not p.exists():
        raise RuntimeError(f"vcvars64.bat not found at {p}")
    return p


def msbuild_command(configuration: str = "Release") -> list[str]:
    return [str(find_msbuild()), str(config.MOD_PROJECT),
            f"/p:Configuration={configuration}", "/p:Platform=x64",
            "/m", "/nologo", "/v:minimal"]


def build_mod() -> BuildResult:
    p = subprocess.run(msbuild_command(), capture_output=True, text=True)
    out = p.stdout + p.stderr
    ok = p.returncode == 0 and config.MOD_DLL.exists()
    return BuildResult(ok, out, config.MOD_DLL if ok else None)


def build_native(name: str, sources: list[Path], out_dir: Path,
                 extra_flags: list[str] = ()) -> BuildResult:
    out_dir.mkdir(parents=True, exist_ok=True)
    obj = out_dir / f"{name}.obj.d"
    obj.mkdir(exist_ok=True)
    exe = out_dir / f"{name}.exe"
    srcs = " ".join(f'"{s.resolve()}"' for s in sources)
    flags = " ".join(extra_flags)
    bat = out_dir / f"build_{name}.bat"
    bat.write_text(
        "@echo off\r\n"
        f'call "{find_vcvars()}" >nul\r\n'
        f'cd /d "{obj}"\r\n'
        f'cl /nologo /std:c++20 /EHsc /O2 /Zi /MT {flags} {srcs} '
        f'/Fe:"{exe}" /link /DEBUG\r\n')
    p = subprocess.run(["cmd", "/c", str(bat)], capture_output=True, text=True)
    out = p.stdout + p.stderr
    ok = p.returncode == 0 and exe.exists()
    return BuildResult(ok, out, exe if ok else None)
```

Create empty `debugloop/__init__.py` and `debugloop/tests/__init__.py`.

Run: `python -m pytest debugloop/tests/test_build.py -v` → 4 PASS.

- [ ] **Step 6: Prove the mod builds at baseline**

Run: `python -c "from debugloop import build; r=build.build_mod(); print(r.ok); print(r.output[-3000:])"`
Expected: `True`. If `False`: read the output, fix only what blocks the build (missing include path, toolset version), and note it in the commit message. If the DLL lands somewhere other than `config.MOD_DLL`, update `MOD_DLL`.

- [ ] **Step 7: `.gitignore`**

Append:
```
debugloop/runs/
debugloop/state/
debugloop/native/
debugloop/ghidra/out/
__pycache__/
.pytest_cache/
```

- [ ] **Step 8: Commit**

```bash
git add .gitignore debugloop/
git commit -m "feat(debugloop): package skeleton, config, build helper"
```

---

### Task 2: Scenarios

**Files:**
- Create: `debugloop/scenario.py`, `debugloop/tests/test_scenario.py`, `debugloop/scenarios/*.toml`, `debugloop/scenarios/selftest/*.toml`

**Interfaces:**
- Consumes: `config.SCENARIOS_DIR`
- Produces:
  - `ProcessSpec(name: str, role: Literal["server","client","solo"], args: list[str])`
  - `Scenario(name: str, step: int, smoke: bool, time_limit_s: int, pass_when: Literal["survive","match_end"], required_passes: int, expect_map: str | None, processes: list[ProcessSpec], path: Path)`
  - `parse(text: str, path: Path) -> Scenario` (raises `ScenarioError`)
  - `load(path: Path) -> Scenario`, `load_all(dir: Path = SCENARIOS_DIR) -> list[Scenario]` (top level only, not `selftest/`)
  - `ladder(step: int, smoke: bool = False) -> list[Scenario]` (sorted by name)
  - `find_scenario(name: str) -> Scenario` (searches top level and `selftest/`)

- [ ] **Step 1: Write the failing test `debugloop/tests/test_scenario.py`**

```python
from pathlib import Path
import pytest
from debugloop import scenario

GOOD = """
name = "s1-dojo-1client"
step = 1
time_limit_s = 900
pass_when = "survive"
required_passes = 3
expect_map = "Dojo_P"

[[process]]
name = "server"
role = "server"
args = ["-rbservermap=Dojo_P"]

[[process]]
name = "c1"
role = "client"
args = []
"""


def test_parse_good():
    s = scenario.parse(GOOD, Path("x.toml"))
    assert s.step == 1 and s.smoke is False
    assert [p.role for p in s.processes] == ["server", "client"]
    assert s.expect_map == "Dojo_P"


@pytest.mark.parametrize("bad,msg", [
    (GOOD.replace('pass_when = "survive"', 'pass_when = "maybe"'), "pass_when"),
    (GOOD.replace('role = "client"', 'role = "ghost"'), "role"),
    (GOOD.replace("step = 1", "step = 9"), "step"),
    (GOOD.replace('name = "c1"', 'name = "server"'), "duplicate"),
])
def test_parse_rejects(bad, msg):
    with pytest.raises(scenario.ScenarioError, match=msg):
        scenario.parse(bad, Path("x.toml"))


def test_client_needs_server():
    text = GOOD.split("[[process]]")[0] + '[[process]]\nname="c1"\nrole="client"\nargs=[]\n'
    with pytest.raises(scenario.ScenarioError, match="server"):
        scenario.parse(text, Path("x.toml"))


def test_real_scenarios_load_and_cover_ladder():
    all_ = scenario.load_all()
    for step in range(4):
        assert scenario.ladder(step), f"no scenario for step {step}"
        assert scenario.ladder(step, smoke=True), f"no smoke scenario for step {step}"
    assert scenario.find_scenario("selftest-crash").step == 0
```

Run: `python -m pytest debugloop/tests/test_scenario.py -v` → FAIL (import error).

- [ ] **Step 2: Write `debugloop/scenario.py`**

```python
import tomllib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Literal

from . import config

ROLES = ("server", "client", "solo")


class ScenarioError(ValueError):
    pass


@dataclass
class ProcessSpec:
    name: str
    role: Literal["server", "client", "solo"]
    args: list[str] = field(default_factory=list)


@dataclass
class Scenario:
    name: str
    step: int
    smoke: bool
    time_limit_s: int
    pass_when: Literal["survive", "match_end"]
    required_passes: int
    expect_map: str | None
    processes: list[ProcessSpec]
    path: Path


def parse(text: str, path: Path) -> Scenario:
    try:
        d = tomllib.loads(text)
    except tomllib.TOMLDecodeError as e:
        raise ScenarioError(f"{path}: bad TOML: {e}") from e
    def need(k, t):
        if k not in d or not isinstance(d[k], t):
            raise ScenarioError(f"{path}: missing or wrong type: {k}")
        return d[k]
    name = need("name", str)
    step = need("step", int)
    if not 0 <= step <= 3:
        raise ScenarioError(f"{path}: step must be 0-3, got {step}")
    pass_when = d.get("pass_when", "survive")
    if pass_when not in ("survive", "match_end"):
        raise ScenarioError(f"{path}: pass_when must be survive|match_end")
    procs = []
    for p in d.get("process", []):
        if p.get("role") not in ROLES:
            raise ScenarioError(f"{path}: bad role {p.get('role')!r}")
        procs.append(ProcessSpec(p["name"], p["role"], list(p.get("args", []))))
    if not procs:
        raise ScenarioError(f"{path}: no [[process]]")
    names = [p.name for p in procs]
    if len(set(names)) != len(names):
        raise ScenarioError(f"{path}: duplicate process name")
    roles = [p.role for p in procs]
    if "client" in roles and roles.count("server") != 1:
        raise ScenarioError(f"{path}: clients need exactly one server")
    return Scenario(name, step, bool(d.get("smoke", False)), need("time_limit_s", int),
                    pass_when, int(d.get("required_passes", 1)), d.get("expect_map"),
                    procs, path)


def load(path: Path) -> Scenario:
    return parse(path.read_text(encoding="utf-8"), path)


def load_all(directory: Path = config.SCENARIOS_DIR) -> list[Scenario]:
    return [load(p) for p in sorted(directory.glob("*.toml"))]


def ladder(step: int, smoke: bool = False) -> list[Scenario]:
    return sorted((s for s in load_all() if s.step == step and s.smoke == smoke),
                  key=lambda s: s.name)


def find_scenario(name: str) -> Scenario:
    for p in list(config.SCENARIOS_DIR.glob("*.toml")) + list(
            (config.SCENARIOS_DIR / "selftest").glob("*.toml")):
        s = load(p)
        if s.name == name:
            return s
    raise ScenarioError(f"no scenario named {name}")
```

- [ ] **Step 3: Write the scenario files**

Map names come from `reborn/Constants.hpp` map lists (lines ~300–364). If a name below is not in that list, use the closest entry from it and keep the file name.

`debugloop/scenarios/s0-solo-dojo.toml`:
```toml
name = "s0-solo-dojo"
step = 0
time_limit_s = 300
pass_when = "survive"
required_passes = 3
expect_map = "Dojo_P"

[[process]]
name = "solo"
role = "solo"
args = ["-rbautopilot", "-rbsolomap=Dojo_P", "-rbcharacter=Rath"]
```

`debugloop/scenarios/s0-solo-dojo-smoke.toml`: same, with `name = "s0-solo-dojo-smoke"`, `smoke = true`, `time_limit_s = 120`, `required_passes = 1`.

`debugloop/scenarios/s1-dojo-1client.toml`:
```toml
name = "s1-dojo-1client"
step = 1
time_limit_s = 900
pass_when = "survive"
required_passes = 3
expect_map = "Dojo_P"

[[process]]
name = "server"
role = "server"
args = ["-rbservermap=Dojo_P"]

[[process]]
name = "c1"
role = "client"
args = ["-rbautopilot", "-rbcharacter=Rath"]
```

`debugloop/scenarios/s1-dojo-1client-smoke.toml`: same, `name = "s1-dojo-1client-smoke"`, `smoke = true`, `time_limit_s = 180`, `required_passes = 1`.

`debugloop/scenarios/s2-algorithm-2clients.toml`:
```toml
name = "s2-algorithm-2clients"
step = 2
time_limit_s = 1800
pass_when = "survive"
required_passes = 2
expect_map = "Caverns_P"

[[process]]
name = "server"
role = "server"
args = ["-rbservermap=Caverns_P"]

[[process]]
name = "c1"
role = "client"
args = ["-rbautopilot", "-rbcharacter=Rath", "-rbseed=1"]

[[process]]
name = "c2"
role = "client"
args = ["-rbautopilot", "-rbcharacter=Oscar Mike", "-rbseed=2"]
```

`debugloop/scenarios/s2-algorithm-2clients-smoke.toml`: same, `name = "s2-algorithm-2clients-smoke"`, `smoke = true`, `time_limit_s = 300`, `required_passes = 1`.

`debugloop/scenarios/s3-meltdown-2clients-bots.toml`:
```toml
name = "s3-meltdown-2clients-bots"
step = 3
time_limit_s = 2400
pass_when = "match_end"
required_passes = 2
expect_map = "IceScort_P"

[[process]]
name = "server"
role = "server"
args = ["-rbservermap=IceScort_P?SpawnBotsTeamA=4?SpawnBotsTeamB=4"]

[[process]]
name = "c1"
role = "client"
args = ["-rbautopilot", "-rbcharacter=Rath", "-rbseed=1"]

[[process]]
name = "c2"
role = "client"
args = ["-rbautopilot", "-rbcharacter=Oscar Mike", "-rbseed=2"]
```

`debugloop/scenarios/s3-meltdown-2clients-bots-smoke.toml`: same, `name = "s3-meltdown-2clients-bots-smoke"`, `smoke = true`, `pass_when = "survive"`, `time_limit_s = 300`, `required_passes = 1`.

`debugloop/scenarios/selftest/selftest-crash.toml`:
```toml
name = "selftest-crash"
step = 0
time_limit_s = 180
pass_when = "survive"
expect_map = "Dojo_P"

[[process]]
name = "solo"
role = "solo"
args = ["-rbautopilot", "-rbsolomap=Dojo_P", "-rbtestcrash"]
```

`debugloop/scenarios/selftest/selftest-hang.toml`: same with `name = "selftest-hang"` and args `["-rbautopilot", "-rbsolomap=Dojo_P", "-rbtesthang", "-rbhangsecs=15"]`.

- [ ] **Step 4: Run tests**

Run: `python -m pytest debugloop/tests/test_scenario.py -v` → all PASS.

- [ ] **Step 5: Commit**

```bash
git add debugloop/scenario.py debugloop/tests/test_scenario.py debugloop/scenarios
git commit -m "feat(debugloop): scenario files and loader"
```

---

### Task 3: Grading and bug signatures

**Files:**
- Create: `debugloop/outcome.py`, `debugloop/signature.py`, `debugloop/tests/test_outcome.py`, `debugloop/tests/test_signature.py`

**Interfaces:**
- Consumes: `scenario.Scenario`
- Produces:
  - `Sample(t: float, name: str, state: dict | None, http_status: int | None)` — one poll of one process's `/state` (`state` is the parsed JSON; `None` when unreachable).
  - `ProcessRecord(name: str, role: str, exit_code: int | None, crash_reports: list[dict], hang_report: dict | None, log_tail: list[str])` — `crash_reports` are parsed `<instance>.<n>.crash.json`; `hang_report` is parsed `<instance>.hang.json`.
  - `Outcome(kind: str, detail: str, process: str | None, phase: str, frame: str | None, code: str | None)`; `kind` ∈ `pass, crash, hang, exit, disconnect, desync, timeout, harness_error`.
  - `classify(scn: Scenario, samples: list[Sample], procs: list[ProcessRecord], elapsed_s: float, match_ended: bool) -> Outcome`
  - `signature.make(o: Outcome, scn: Scenario) -> str | None` (None for pass/harness_error); `signature.slug(sig: str) -> str` (filesystem-safe).
- Crash report JSON (written by Task 6): `{"code": "0xC0000005", "address": "battleborn+0x1A2B3C", "frames": ["battleborn+0x1A2B3C", "reborn+0x4567", ...], "thread": 1234, "first_chance": true, "dump": "solo.1.dmp"}`.
- Hang report JSON: `{"seconds": 60, "frames": [...], "dump": "solo.hang.dmp"}`.
- `/state` JSON: see Task 7.

Phase rules (`Outcome.phase`): `startup` until every process reports `ticks>0` and (clients/solo) `has_pawn` true and (server) `listening` true; then `playing`.

Grading order (first match wins):
1. Any crash report that is second-chance, or first-chance from a process that has exited → `crash` (process with the earliest report).
2. Any hang report → `hang`.
3. Last 3 samples of one process are `http_status == 503` → `hang` (frame `unknown`).
4. Any process exited (`exit_code is not None`):
   - server exited 0 and `match_ended` and `pass_when == "match_end"` → `pass`
   - else → `exit` with code.
5. In phase `playing`, a client sample with `state["connected"] is False` → `disconnect` (side `client`, category from `state["disconnect_reason"]` or `unknown`); a server sample whose `connections` drops below the client count → `disconnect` (side `server`, category `dropped`).
6. Desync: for a client with a pawn, compare its `pawn_location` with the server's `player_locations[<client instance>]`; distance > 1500 for 5 consecutive paired samples → `desync`.
7. `expect_map` set and a playing process reports another `map` → `exit` with detail `wrong_map`, code `wrong_map`.
8. Still `startup` after 240 s → `timeout` (phase `startup`).
9. `elapsed_s >= time_limit_s`: `pass_when == "survive"` → `pass`; else → `timeout` (phase `playing`).
10. Otherwise `Outcome("running", ...)` (the runner keeps going).

Signature rules:
- Frame normalization: lower-case; `serverborn+` → `battleborn+`; choose the first frame not in `ntdll`, `kernelbase`, `kernel32`, `ucrtbase`, `vcruntime140`; if that frame is in `reborn+`, keep it **and** append the first `battleborn+` frame after it (`reborn+0x4567>battleborn+0x1a2b3c`); reborn offsets are rounded down to 0x100 to survive small rebuild shifts.
- `crash:<code>:<frame>`, `hang:<frame>`, `exit:<code>`, `disconnect:<side>:<category>`, `desync:<map>`, `timeout:<phase>`.

- [ ] **Step 1: Write the failing tests**

`debugloop/tests/test_outcome.py`:
```python
from pathlib import Path
from debugloop import outcome, scenario
from debugloop.outcome import Sample, ProcessRecord

def scn(pass_when="survive", limit=900, expect_map="Dojo_P"):
    text = f'''
name="t"
step=1
time_limit_s={limit}
pass_when="{pass_when}"
expect_map="{expect_map}"
[[process]]
name="server"
role="server"
args=[]
[[process]]
name="c1"
role="client"
args=[]
'''
    return scenario.parse(text, Path("t.toml"))

def srv(**kw):
    s = {"ticks": 10, "listening": True, "map": "Dojo_P", "connections": 1,
         "player_locations": {"c1": [0, 0, 0]}}
    s.update(kw); return s

def cli(**kw):
    s = {"ticks": 10, "has_pawn": True, "connected": True, "map": "Dojo_P",
         "pawn_location": [0, 0, 0]}
    s.update(kw); return s

def recs(**over):
    base = {"server": ProcessRecord("server", "server", None, [], None, []),
            "c1": ProcessRecord("c1", "client", None, [], None, [])}
    base.update(over); return list(base.values())

def pair(t, s=None, c=None):
    return [Sample(t, "server", s or srv(), 200), Sample(t, "c1", c or cli(), 200)]

def test_survive_to_time_limit_passes():
    o = outcome.classify(scn(), pair(1), recs(), 900, False)
    assert o.kind == "pass"

def test_clean_exit_mid_run_is_not_pass():
    r = recs(server=ProcessRecord("server", "server", 0, [], None, []))
    o = outcome.classify(scn(), pair(1), r, 100, False)
    assert o.kind == "exit" and o.code == "0"

def test_match_end_exit_zero_passes():
    r = recs(server=ProcessRecord("server", "server", 0, [], None, []))
    o = outcome.classify(scn("match_end"), pair(1), r, 100, True)
    assert o.kind == "pass"

def test_crash_wins_over_exit():
    rep = {"code": "0xC0000005", "frames": ["battleborn+0x10"], "first_chance": True}
    r = recs(c1=ProcessRecord("c1", "client", 3, [rep], None, []))
    o = outcome.classify(scn(), pair(1), r, 50, False)
    assert o.kind == "crash" and o.process == "c1" and o.code == "0xC0000005"

def test_first_chance_report_on_live_process_is_ignored():
    rep = {"code": "0xC0000005", "frames": ["battleborn+0x10"], "first_chance": True}
    r = recs(c1=ProcessRecord("c1", "client", None, [rep], None, []))
    assert outcome.classify(scn(), pair(1), r, 50, False).kind == "running"

def test_three_503s_is_hang():
    s = pair(1) + [Sample(t, "c1", None, 503) for t in (3, 5, 7)]
    o = outcome.classify(scn(), s, recs(), 60, False)
    assert o.kind == "hang" and o.frame == "unknown"

def test_client_disconnect_in_play():
    s = pair(1) + pair(3, c=cli(connected=False, disconnect_reason="timeout"))
    o = outcome.classify(scn(), s, recs(), 60, False)
    assert (o.kind, o.detail) == ("disconnect", "client:timeout")

def test_desync_needs_five_samples():
    far = cli(pawn_location=[2000, 0, 0])
    s = pair(1) + [x for t in range(4) for x in pair(3 + 2 * t, c=far)]
    assert outcome.classify(scn(), s, recs(), 60, False).kind == "running"
    s += pair(11, c=far)
    assert outcome.classify(scn(), s, recs(), 60, False).kind == "desync"

def test_startup_timeout():
    s = [Sample(1, "server", srv(), 200), Sample(1, "c1", cli(has_pawn=False), 200)]
    o = outcome.classify(scn(), s, recs(), 241, False)
    assert (o.kind, o.phase) == ("timeout", "startup")

def test_wrong_map():
    o = outcome.classify(scn(), pair(1, c=cli(map="Frontend")), recs(), 60, False)
    assert o.kind == "exit" and o.code == "wrong_map"
```

`debugloop/tests/test_signature.py`:
```python
from debugloop import signature
from debugloop.outcome import Outcome
from debugloop.tests.test_outcome import scn

def O(kind, **kw):
    d = dict(detail="", process="c1", phase="playing", frame=None, code=None); d.update(kw)
    return Outcome(kind, **d)

def test_crash_skips_system_frames_and_normalizes_server():
    o = O("crash", code="0xC0000005", frame="NTDLL+0x1|SERVERBORN+0x1A2B3C")
    assert signature.make(o, scn()) == "crash:0xc0000005:battleborn+0x1a2b3c"

def test_reborn_frame_rounded_and_paired():
    o = O("crash", code="0xC0000005", frame="reborn+0x4567|battleborn+0x99")
    assert signature.make(o, scn()) == "crash:0xc0000005:reborn+0x4500>battleborn+0x99"

def test_other_kinds():
    assert signature.make(O("exit", code="3"), scn()) == "exit:3"
    assert signature.make(O("disconnect", detail="client:timeout"), scn()) == "disconnect:client:timeout"
    assert signature.make(O("desync"), scn()) == "desync:Dojo_P"
    assert signature.make(O("timeout", phase="startup"), scn()) == "timeout:startup"
    assert signature.make(O("pass"), scn()) is None

def test_slug_is_filename_safe():
    assert signature.slug("crash:0xc0000005:reborn+0x4500>battleborn+0x99") == \
        "crash_0xc0000005_reborn+0x4500_battleborn+0x99"
```

`Outcome.frame` carries the frame list joined with `|` (the classifier joins the report's `frames`), so the signature module does the choosing.

Run: `python -m pytest debugloop/tests/test_outcome.py debugloop/tests/test_signature.py -v` → FAIL (import errors).

- [ ] **Step 2: Write `debugloop/outcome.py`**

```python
import math
from dataclasses import dataclass, field

from .scenario import Scenario

DESYNC_UNITS = 1500
DESYNC_SAMPLES = 5
STARTUP_TIMEOUT_S = 240


@dataclass
class Sample:
    t: float
    name: str
    state: dict | None
    http_status: int | None


@dataclass
class ProcessRecord:
    name: str
    role: str
    exit_code: int | None
    crash_reports: list[dict] = field(default_factory=list)
    hang_report: dict | None = None
    log_tail: list[str] = field(default_factory=list)


@dataclass
class Outcome:
    kind: str
    detail: str = ""
    process: str | None = None
    phase: str = "startup"
    frame: str | None = None
    code: str | None = None


def _ready(role: str, st: dict | None) -> bool:
    if not st or st.get("ticks", 0) <= 0:
        return False
    if role == "server":
        return bool(st.get("listening"))
    return bool(st.get("has_pawn"))


def _phase(scn: Scenario, samples: list[Sample]) -> tuple[str, int]:
    """Return phase and index of the first sample batch where all were ready."""
    roles = {p.name: p.role for p in scn.processes}
    ready: set[str] = set()
    for i, s in enumerate(samples):
        if _ready(roles.get(s.name, ""), s.state):
            ready.add(s.name)
        if ready >= set(roles):
            return "playing", i
    return "startup", len(samples)


def classify(scn: Scenario, samples: list[Sample], procs: list[ProcessRecord],
             elapsed_s: float, match_ended: bool) -> Outcome:
    phase, start = _phase(scn, samples)
    roles = {p.name: p.role for p in scn.processes}

    # A first-chance report only counts if the process then died: the game (or its
    # anti-tamper) may raise and handle access violations on purpose.
    crashed = [(r, c) for r in procs for c in r.crash_reports
               if r.exit_code is not None or not c.get("first_chance", False)]
    if crashed:
        r, c = crashed[0]
        return Outcome("crash", c.get("address", ""), r.name, phase,
                       "|".join(c.get("frames", [])) or "unknown", c.get("code"))
    for r in procs:
        if r.hang_report:
            return Outcome("hang", f"{r.hang_report.get('seconds')}s", r.name, phase,
                           "|".join(r.hang_report.get("frames", [])) or "unknown")
    for name in roles:
        last = [s for s in samples if s.name == name][-3:]
        if len(last) == 3 and all(s.http_status == 503 for s in last):
            return Outcome("hang", "game_thread_unresponsive", name, phase, "unknown")
    for r in procs:
        if r.exit_code is not None:
            if (r.role == "server" and r.exit_code == 0 and match_ended
                    and scn.pass_when == "match_end"):
                return Outcome("pass", "match_end", r.name, phase)
            return Outcome("exit", "", r.name, phase, code=str(r.exit_code))

    if phase == "playing":
        play = samples[start:]
        n_clients = sum(1 for v in roles.values() if v == "client")
        for s in play:
            if not s.state:
                continue
            if roles.get(s.name) == "client" and s.state.get("connected") is False:
                cat = s.state.get("disconnect_reason") or "unknown"
                return Outcome("disconnect", f"client:{cat}", s.name, phase)
            if roles.get(s.name) == "server" and s.state.get("connections", n_clients) < n_clients:
                return Outcome("disconnect", "server:dropped", s.name, phase)
        d = _desync(roles, play)
        if d:
            return Outcome("desync", d, d, phase)
        for s in play:
            if (scn.expect_map and s.state and s.state.get("map")
                    and s.state["map"] != scn.expect_map):
                return Outcome("exit", f"wrong_map {s.state['map']}", s.name, phase,
                               code="wrong_map")
    elif elapsed_s >= STARTUP_TIMEOUT_S:
        return Outcome("timeout", "", None, "startup")

    if elapsed_s >= scn.time_limit_s:
        if scn.pass_when == "survive" and phase == "playing":
            return Outcome("pass", "survived", None, phase)
        return Outcome("timeout", "", None, phase)
    return Outcome("running", "", None, phase)


def _desync(roles: dict[str, str], play: list[Sample]) -> str | None:
    server_name = next((n for n, r in roles.items() if r == "server"), None)
    if not server_name:
        return None
    streak: dict[str, int] = {}
    last_server: dict | None = None
    for s in play:
        if s.name == server_name:
            last_server = s.state
            continue
        if roles.get(s.name) != "client" or not s.state or not last_server:
            continue
        mine = s.state.get("pawn_location")
        theirs = (last_server.get("player_locations") or {}).get(s.name)
        if not mine or not theirs:
            continue
        if math.dist(mine, theirs) > DESYNC_UNITS:
            streak[s.name] = streak.get(s.name, 0) + 1
            if streak[s.name] >= DESYNC_SAMPLES:
                return s.name
        else:
            streak[s.name] = 0
    return None
```

Note on test `test_survive_to_time_limit_passes`: with only one sample pair the phase is already `playing` (both ready), so survive → pass.

- [ ] **Step 3: Write `debugloop/signature.py`**

```python
import re

from .outcome import Outcome
from .scenario import Scenario

SYSTEM = ("ntdll", "kernelbase", "kernel32", "ucrtbase", "vcruntime140", "msvcp140")


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
        return _round_reborn(first) + (f">{nxt}" if nxt else "")
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
    if o.kind == "timeout":
        return f"timeout:{o.phase}"
    return f"{o.kind}:unknown"


def slug(sig: str) -> str:
    return re.sub(r"[^a-z0-9+.]+", "_", sig.lower()).strip("_")
```

- [ ] **Step 4: Run tests**

Run: `python -m pytest debugloop/tests/test_outcome.py debugloop/tests/test_signature.py -v` → all PASS.

- [ ] **Step 5: Commit**

```bash
git add debugloop/outcome.py debugloop/signature.py debugloop/tests/test_outcome.py debugloop/tests/test_signature.py
git commit -m "feat(debugloop): run grading and bug signatures"
```

---

### Task 4: Bug ledger and loop state

**Files:**
- Create: `debugloop/ledger.py`, `debugloop/state.py`, `debugloop/tests/test_ledger.py`, `debugloop/tests/test_state.py`

**Interfaces:**
- Consumes: `config.STATE_DIR`
- Produces:
  - `Bug(signature: str, first_seen: str, last_seen: str, count: int, scenarios: list[str], status: Literal["open","fixing","fixed","gave_up"], attempts: int, runs: list[str], notes: list[str])`
  - `Ledger.load(dir: Path = STATE_DIR) -> Ledger`; `.record(sig, scenario, run_id) -> Bug`; `.get(sig) -> Bug | None`; `.set_status(sig, status, note="")`; `.open_bugs() -> list[Bug]` (by count desc); `.save()` writes `ledger.json` and `ledger.md` atomically.
  - `LoopState(step=0, consecutive_passes=0, scenario_index=0, current_bug=None, bug_scenario=None, attempts_on_current=0, harness_errors_in_row=0, stopped_reason=None, bug_phase=None, bug_elapsed_s=0.0)`; `LoopState.load(dir=STATE_DIR) -> LoopState` (missing file → defaults; corrupt → raises `StateCorrupt`); `.save(dir=STATE_DIR)` atomic.
  - `atomic_write(path: Path, text: str)` in `state.py`, reused by `ledger.py`.

- [ ] **Step 1: Write the failing tests**

`debugloop/tests/test_state.py`:
```python
import pytest
from debugloop import state

def test_roundtrip(tmp_path):
    s = state.LoopState(step=2, consecutive_passes=1, current_bug="exit:3")
    s.save(tmp_path)
    assert state.LoopState.load(tmp_path) == s

def test_missing_gives_defaults(tmp_path):
    assert state.LoopState.load(tmp_path) == state.LoopState()

def test_corrupt_raises_not_resets(tmp_path):
    (tmp_path / "loop_state.json").write_text("{half")
    with pytest.raises(state.StateCorrupt):
        state.LoopState.load(tmp_path)

def test_atomic_write_leaves_no_temp(tmp_path):
    state.atomic_write(tmp_path / "a.json", "{}")
    assert [p.name for p in tmp_path.iterdir()] == ["a.json"]
```

`debugloop/tests/test_ledger.py`:
```python
from debugloop import ledger

def test_record_counts_and_persists(tmp_path):
    L = ledger.Ledger.load(tmp_path)
    L.record("exit:3", "s1", "run1")
    b = L.record("exit:3", "s2", "run2")
    assert b.count == 2 and b.scenarios == ["s1", "s2"] and b.status == "open"
    L.save()
    L2 = ledger.Ledger.load(tmp_path)
    assert L2.get("exit:3").runs == ["run1", "run2"]
    assert "exit:3" in (tmp_path / "ledger.md").read_text()

def test_fixed_bug_reopens_on_recurrence(tmp_path):
    L = ledger.Ledger.load(tmp_path)
    L.record("exit:3", "s1", "r1")
    L.set_status("exit:3", "fixed", "patched X")
    b = L.record("exit:3", "s1", "r2")
    assert b.status == "open" and any("reopened" in n for n in b.notes)

def test_open_bugs_sorted_by_count(tmp_path):
    L = ledger.Ledger.load(tmp_path)
    L.record("a", "s", "r"); L.record("b", "s", "r"); L.record("b", "s", "r2")
    assert [b.signature for b in L.open_bugs()] == ["b", "a"]
```

Run: `python -m pytest debugloop/tests/test_state.py debugloop/tests/test_ledger.py -v` → FAIL.

- [ ] **Step 2: Write `debugloop/state.py`**

```python
import json
import os
from dataclasses import asdict, dataclass
from pathlib import Path

from . import config

FILE = "loop_state.json"


class StateCorrupt(RuntimeError):
    pass


def atomic_write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


@dataclass
class LoopState:
    step: int = 0
    consecutive_passes: int = 0
    scenario_index: int = 0
    current_bug: str | None = None
    bug_scenario: str | None = None
    attempts_on_current: int = 0
    harness_errors_in_row: int = 0
    stopped_reason: str | None = None
    bug_phase: str | None = None          # phase the current bug happened in
    bug_elapsed_s: float = 0.0            # how far into the run it happened

    @classmethod
    def load(cls, directory: Path = config.STATE_DIR) -> "LoopState":
        p = directory / FILE
        if not p.exists():
            return cls()
        try:
            return cls(**json.loads(p.read_text(encoding="utf-8")))
        except (json.JSONDecodeError, TypeError) as e:
            raise StateCorrupt(f"{p} is unreadable: {e}. Fix or delete it by hand.") from e

    def save(self, directory: Path = config.STATE_DIR) -> None:
        atomic_write(directory / FILE, json.dumps(asdict(self), indent=2))
```

- [ ] **Step 3: Write `debugloop/ledger.py`**

```python
import json
from dataclasses import asdict, dataclass, field
from datetime import datetime
from pathlib import Path

from . import config
from .state import atomic_write


def _now() -> str:
    return datetime.now().isoformat(timespec="seconds")


@dataclass
class Bug:
    signature: str
    first_seen: str
    last_seen: str
    count: int = 0
    scenarios: list[str] = field(default_factory=list)
    status: str = "open"
    attempts: int = 0
    runs: list[str] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)


class Ledger:
    def __init__(self, directory: Path, bugs: dict[str, Bug]):
        self.dir = directory
        self.bugs = bugs

    @classmethod
    def load(cls, directory: Path = config.STATE_DIR) -> "Ledger":
        p = directory / "ledger.json"
        bugs = {}
        if p.exists():
            bugs = {k: Bug(**v) for k, v in json.loads(p.read_text(encoding="utf-8")).items()}
        return cls(directory, bugs)

    def get(self, sig: str) -> Bug | None:
        return self.bugs.get(sig)

    def record(self, sig: str, scenario: str, run_id: str) -> Bug:
        b = self.bugs.get(sig)
        if b is None:
            b = self.bugs[sig] = Bug(sig, _now(), _now())
        if b.status == "fixed":
            b.status = "open"
            b.notes.append(f"{_now()} reopened by {run_id}")
        b.count += 1
        b.last_seen = _now()
        if scenario not in b.scenarios:
            b.scenarios.append(scenario)
        b.runs.append(run_id)
        return b

    def set_status(self, sig: str, status: str, note: str = "") -> None:
        b = self.bugs[sig]
        b.status = status
        if note:
            b.notes.append(f"{_now()} {status}: {note}")

    def open_bugs(self) -> list[Bug]:
        return sorted((b for b in self.bugs.values() if b.status in ("open", "fixing")),
                      key=lambda b: -b.count)

    def save(self) -> None:
        atomic_write(self.dir / "ledger.json",
                     json.dumps({k: asdict(v) for k, v in self.bugs.items()}, indent=2))
        lines = ["# Bug ledger", "", "| Status | Count | Signature | Scenarios | Last seen |",
                 "|---|---|---|---|---|"]
        for b in sorted(self.bugs.values(), key=lambda b: (b.status, -b.count)):
            lines.append(f"| {b.status} | {b.count} | `{b.signature}` | "
                         f"{', '.join(b.scenarios)} | {b.last_seen} |")
        atomic_write(self.dir / "ledger.md", "\n".join(lines) + "\n")
```

- [ ] **Step 4: Run tests**

Run: `python -m pytest debugloop/tests -v` → all PASS.

- [ ] **Step 5: Commit**

```bash
git add debugloop/ledger.py debugloop/state.py debugloop/tests/test_ledger.py debugloop/tests/test_state.py
git commit -m "feat(debugloop): bug ledger and atomic loop state"
```

---

### Task 5: C++ launch flags

**Files:**
- Create: `reborn/LaunchOptions.hpp`, `reborn/LaunchOptions.cpp`, `reborn/tests/test_launch_options.cpp`, `debugloop/tests/test_native.py`

**Interfaces:**
- Produces (C++): `LaunchOptions::Options` (fields below), `LaunchOptions::Parse(const std::wstring&) -> Options`, `LaunchOptions::InitFromCommandLine()`, `LaunchOptions::Get() -> const Options&`.
- Produces (Python test helper): `test_native.build_and_run(name, sources, args=()) -> subprocess.CompletedProcess`.
- Flags: `-rbinstance=NAME` `-rbdebugport=N` `-rbrundir=PATH` `-rbautopilot` `-rbjoin=IP:PORT` `-rbsolomap=MAP` `-rbservermap=URL` `-rbplayers=N` `-rbcharacter=NAME` `-rbseed=N` `-rbhangsecs=N` `-rbtesthang` `-rbtestcrash`. Bad values go to `errors`, unknown `-rb*` flags to `unknown`; other arguments are ignored (they belong to the game).

The project already ships `reborn/httplib.h` and `reborn/json.hpp` (nlohmann); use those, add no new libraries.

- [ ] **Step 1: Write the failing native test `reborn/tests/test_launch_options.cpp`**

```cpp
#include "../LaunchOptions.hpp"
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

int main() {
    auto o = LaunchOptions::Parse(LR"(Battleborn.exe -windowed -rbinstance=c1 -rbdebugport=18081 -rbautopilot "-rbcharacter=Oscar Mike" -rbjoin=127.0.0.1:7777 -rbseed=7 -rbhangsecs=15 -rbtesthang)");
    CHECK(o.instance == L"c1");
    CHECK(o.debugPort == 18081);
    CHECK(o.autopilot);
    CHECK(o.character == "Oscar Mike");
    CHECK(o.join == L"127.0.0.1:7777");
    CHECK(o.seed == 7);
    CHECK(o.hangSeconds == 15);
    CHECK(o.testHang && !o.testCrash);
    CHECK(o.errors.empty() && o.unknown.empty());

    auto s = LaunchOptions::Parse(LR"(Serverborn.exe -rbservermap=IceScort_P?SpawnBotsTeamA=4 -rbplayers=2)");
    CHECK(s.serverMap == L"IceScort_P?SpawnBotsTeamA=4");
    CHECK(s.players == 2);

    auto d = LaunchOptions::Parse(L"Battleborn.exe");
    CHECK(d.instance == L"game" && d.debugPort == 0 && d.hangSeconds == 60 && !d.autopilot);

    auto bad = LaunchOptions::Parse(L"Battleborn.exe -rbdebugport=80 -rbhangsecs=abc -rbwhat");
    CHECK(bad.errors.size() == 2);
    CHECK(bad.unknown.size() == 1);
    CHECK(bad.debugPort == 0 && bad.hangSeconds == 60);

    std::puts(failures ? "FAIL" : "PASS");
    return failures;
}
```

- [ ] **Step 2: Write `debugloop/tests/test_native.py`**

```python
import subprocess
from pathlib import Path

import pytest

from debugloop import build, config

SRC = config.REPO / "reborn"
OUT = config.NATIVE_OUT


def build_and_run(name: str, sources: list[Path], args=(), timeout=60):
    res = build.build_native(name, sources, OUT, [f'/I"{SRC}"'])
    assert res.ok, res.output[-4000:]
    return subprocess.run([str(res.artifact), *args], capture_output=True, text=True,
                          timeout=timeout)


def test_launch_options():
    p = build_and_run("test_launch_options",
                      [SRC / "tests" / "test_launch_options.cpp", SRC / "LaunchOptions.cpp"])
    assert p.returncode == 0, p.stdout + p.stderr
    assert "PASS" in p.stdout
```

Run: `python -m pytest debugloop/tests/test_native.py -v` → FAIL (`LaunchOptions.cpp` missing, compile error).

- [ ] **Step 3: Write `reborn/LaunchOptions.hpp`**

```cpp
#pragma once
#include <string>
#include <vector>

namespace LaunchOptions {
    struct Options {
        std::wstring instance = L"game";
        int debugPort = 0;            // 0 = no debug server
        std::wstring runDir;          // empty = Documents\RebornLogs\<instance>-<time>
        bool autopilot = false;
        std::wstring join;            // client: "IP:PORT" to connect to
        std::wstring soloMap;         // solo: map to open
        std::wstring serverMap;       // server: travel URL, e.g. "Dojo_P" or "IceScort_P?SpawnBotsTeamA=4"
        int players = 0;              // server: players to wait for (0 = keep default)
        std::string character;        // display name from Constants::CharacterSelectCharacterTable
        unsigned int seed = 0;
        int hangSeconds = 60;
        bool testHang = false;        // freeze the game thread 20 s after play starts
        bool testCrash = false;       // null write 20 s after play starts
        std::vector<std::wstring> errors;
        std::vector<std::wstring> unknown;
    };

    Options Parse(const std::wstring& commandLine);
    void InitFromCommandLine();
    const Options& Get();
}
```

- [ ] **Step 4: Write `reborn/LaunchOptions.cpp`**

```cpp
#include "LaunchOptions.hpp"
#include <Windows.h>
#include <shellapi.h>
#include <climits>
#pragma comment(lib, "shell32.lib")

namespace LaunchOptions {
    static Options g_options;

    static std::string Narrow(const std::wstring& w) {
        if (w.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string s(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
        return s;
    }

    static bool ToInt(const std::wstring& v, int lo, int hi, int& out) {
        try {
            size_t used = 0;
            int n = std::stoi(v, &used);
            if (used != v.size() || n < lo || n > hi) return false;
            out = n;
            return true;
        }
        catch (...) {
            return false;
        }
    }

    Options Parse(const std::wstring& commandLine) {
        Options o;
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(commandLine.c_str(), &argc);
        if (!argv) return o;
        for (int i = 1; i < argc; i++) {
            std::wstring a = argv[i];
            if (a.rfind(L"-rb", 0) != 0) continue;
            std::wstring key = a, val;
            size_t eq = a.find(L'=');
            if (eq != std::wstring::npos) {
                key = a.substr(0, eq);
                val = a.substr(eq + 1);
            }
            int n = 0;
            if (key == L"-rbinstance" && !val.empty()) o.instance = val;
            else if (key == L"-rbdebugport") { if (ToInt(val, 1024, 65535, n)) o.debugPort = n; else o.errors.push_back(a); }
            else if (key == L"-rbrundir" && !val.empty()) o.runDir = val;
            else if (key == L"-rbautopilot") o.autopilot = true;
            else if (key == L"-rbjoin" && !val.empty()) o.join = val;
            else if (key == L"-rbsolomap" && !val.empty()) o.soloMap = val;
            else if (key == L"-rbservermap" && !val.empty()) o.serverMap = val;
            else if (key == L"-rbplayers") { if (ToInt(val, 1, 10, n)) o.players = n; else o.errors.push_back(a); }
            else if (key == L"-rbcharacter" && !val.empty()) o.character = Narrow(val);
            else if (key == L"-rbseed") { if (ToInt(val, 0, INT_MAX, n)) o.seed = (unsigned)n; else o.errors.push_back(a); }
            else if (key == L"-rbhangsecs") { if (ToInt(val, 5, 3600, n)) o.hangSeconds = n; else o.errors.push_back(a); }
            else if (key == L"-rbtesthang") o.testHang = true;
            else if (key == L"-rbtestcrash") o.testCrash = true;
            else o.unknown.push_back(a);
        }
        LocalFree(argv);
        return o;
    }

    void InitFromCommandLine() { g_options = Parse(GetCommandLineW()); }
    const Options& Get() { return g_options; }
}
```

- [ ] **Step 5: Run**

Run: `python -m pytest debugloop/tests/test_native.py -v` → PASS.

- [ ] **Step 6: Commit**

```bash
git add reborn/LaunchOptions.hpp reborn/LaunchOptions.cpp reborn/tests/test_launch_options.cpp debugloop/tests/test_native.py
git commit -m "feat(reborn): -rb* launch flags"
```

---

### Task 6: C++ crash and freeze capture

**Files:**
- Create: `reborn/Diagnostics.hpp`, `reborn/Diagnostics.cpp`, `reborn/tests/crashme.cpp`
- Modify: `debugloop/tests/test_native.py`

**Interfaces:**
- Produces (C++):
  - `Diagnostics::Init(const std::wstring& runDir, const std::wstring& instance, int hangSeconds)` — call once, after the console exists.
  - `Diagnostics::NoteTick()` — call every game tick on the game thread (the first caller becomes "the game thread").
  - `Diagnostics::MillisSinceLastTick() -> long long`, `Diagnostics::TickCount() -> unsigned long long`
  - `Diagnostics::RecentLines() -> std::vector<std::string>` (last 300 console lines)
  - `Diagnostics::LastLineContaining(const std::string&) -> std::string` (empty if none)
  - `Diagnostics::FormatAddress(unsigned long long) -> std::string` → `"module+0xOFFSET"`, module lower-case without extension
  - `Diagnostics::RunDir() -> const std::wstring&`
- Files written to the run dir: `<instance>.log`; `<instance>.<n>.dmp` + `<instance>.<n>.crash.json` (n = 1..5); `<instance>.hang.dmp` + `<instance>.hang.json`. JSON shapes are in Task 3.

- [ ] **Step 1: Write the test program `reborn/tests/crashme.cpp`**

```cpp
// Usage: crashme <mode> <rundir>   mode = crash | hang | spam
#include "../Diagnostics.hpp"
#include <Windows.h>
#include <cstdio>
#include <string>

template <int N> __declspec(noinline) void Fault() { *(volatile int*)(uintptr_t)(N + 8) = N; }

template <int N> void TryFault() {
    __try { Fault<N>(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return 2;
    std::wstring mode = argv[1];
    Diagnostics::Init(argv[2], L"crashme", 5);
    Diagnostics::NoteTick();
    std::printf("hello from crashme\n");
    if (mode == L"crash") {
        Fault<0>();
    } else if (mode == L"hang") {
        Sleep(INFINITE);
    } else if (mode == L"spam") {
        TryFault<1>(); TryFault<1>(); TryFault<1>();   // same address: reported once
        TryFault<2>(); TryFault<3>(); TryFault<4>(); TryFault<5>();
        TryFault<6>(); TryFault<7>(); TryFault<8>();  // over the limit of 5
        Sleep(500);
        return 0;
    }
    return 3;
}
```

- [ ] **Step 2: Add the failing tests to `debugloop/tests/test_native.py`**

```python
import json
import time


def _crashme():
    res = build.build_native("crashme", [SRC / "tests" / "crashme.cpp", SRC / "Diagnostics.cpp"],
                             OUT, [f'/I"{SRC}"'])
    assert res.ok, res.output[-4000:]
    return res.artifact


def test_crash_writes_dump_and_report(tmp_path):
    p = subprocess.run([str(_crashme()), "crash", str(tmp_path)], capture_output=True, text=True,
                       timeout=60)
    assert p.returncode != 0
    reports = sorted(tmp_path.glob("crashme.*.crash.json"))
    assert reports, list(tmp_path.iterdir())
    r = json.loads(reports[0].read_text())
    assert r["code"] == "0xC0000005"
    assert r["frames"][0].startswith("crashme+0x")
    assert (tmp_path / r["dump"]).stat().st_size > 10_000
    assert any("hello from crashme" in line for line in r["log_tail"])
    assert "hello from crashme" in (tmp_path / "crashme.log").read_text()


def test_hang_writes_hang_report(tmp_path):
    proc = subprocess.Popen([str(_crashme()), "hang", str(tmp_path)])
    try:
        deadline = time.time() + 30
        while time.time() < deadline and not (tmp_path / "crashme.hang.json").exists():
            time.sleep(0.5)
        r = json.loads((tmp_path / "crashme.hang.json").read_text())
        assert r["seconds"] == 5
        assert any(f.startswith(("ntdll+", "kernelbase+")) for f in r["frames"])
        assert (tmp_path / "crashme.hang.dmp").exists()
    finally:
        proc.kill()


def test_spam_is_deduplicated_and_capped(tmp_path):
    p = subprocess.run([str(_crashme()), "spam", str(tmp_path)], capture_output=True, text=True,
                       timeout=60)
    assert p.returncode == 0, p.stdout + p.stderr
    reports = list(tmp_path.glob("crashme.*.crash.json"))
    assert len(reports) == 5
    assert all(json.loads(r.read_text())["first_chance"] for r in reports)
```

Run: `python -m pytest debugloop/tests/test_native.py -v -k "crash or hang or spam"` → FAIL (compile error, `Diagnostics.cpp` missing).

- [ ] **Step 3: Write `reborn/Diagnostics.hpp`**

```cpp
#pragma once
#include <string>
#include <vector>

namespace Diagnostics {
    void Init(const std::wstring& runDir, const std::wstring& instance, int hangSeconds);
    void NoteTick();
    long long MillisSinceLastTick();
    unsigned long long TickCount();
    std::vector<std::string> RecentLines();
    std::string LastLineContaining(const std::string& needle);
    std::string FormatAddress(unsigned long long address);
    const std::wstring& RunDir();
}
```

- [ ] **Step 4: Write `reborn/Diagnostics.cpp`**

```cpp
#include "Diagnostics.hpp"
#include <Windows.h>
#include <DbgHelp.h>
#include <io.h>
#include <fcntl.h>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#pragma comment(lib, "dbghelp.lib")

namespace Diagnostics {
    namespace {
        std::wstring g_runDir, g_instance;
        int g_hangSeconds = 60;
        std::atomic<long long> g_lastTickMs{ 0 };
        std::atomic<unsigned long long> g_ticks{ 0 };
        std::atomic<DWORD> g_gameThreadId{ 0 };
        DWORD g_workerThreadId = 0;

        std::mutex g_linesMutex;
        std::deque<std::string> g_lines;
        constexpr size_t kMaxLines = 300;

        std::mutex g_crashMutex;
        std::set<unsigned long long> g_seenAddresses;
        int g_reports = 0;
        constexpr int kMaxReports = 5;

        struct CrashRequest { EXCEPTION_POINTERS* ep; DWORD threadId; bool firstChance; };
        CrashRequest g_request{};
        HANDLE g_requestEvent = nullptr, g_doneEvent = nullptr;
        LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;

        long long NowMs() { return (long long)GetTickCount64(); }

        std::string Narrow(const std::wstring& w) {
            if (w.empty()) return {};
            int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
            std::string s(n, '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
            return s;
        }

        std::string JsonString(const std::string& s) {
            std::string o = "\"";
            for (unsigned char c : s) {
                if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
                else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += (char)c;
            }
            return o + "\"";
        }

        void PushLine(const std::string& line) {
            std::lock_guard lk(g_linesMutex);
            g_lines.push_back(line);
            if (g_lines.size() > kMaxLines) g_lines.pop_front();
        }

        // Everything printed to stdout/stderr goes to the console, <instance>.log and the ring buffer.
        void StartLogTee() {
            int originalOut = _dup(_fileno(stdout));
            int fds[2];
            if (_pipe(fds, 1 << 20, _O_BINARY) != 0) return;
            std::fflush(stdout);
            std::fflush(stderr);
            _dup2(fds[1], _fileno(stdout));
            _dup2(fds[1], _fileno(stderr));
            std::setvbuf(stdout, nullptr, _IONBF, 0);
            std::setvbuf(stderr, nullptr, _IONBF, 0);
            std::wstring logPath = g_runDir + L"\\" + g_instance + L".log";
            std::thread([readFd = fds[0], originalOut, logPath] {
                FILE* log = _wfopen(logPath.c_str(), L"ab");
                char buf[4096];
                std::string partial;
                for (;;) {
                    int n = _read(readFd, buf, sizeof buf);
                    if (n <= 0) break;
                    if (originalOut >= 0) _write(originalOut, buf, n);
                    if (log) { std::fwrite(buf, 1, n, log); std::fflush(log); }
                    partial.append(buf, n);
                    size_t nl;
                    while ((nl = partial.find('\n')) != std::string::npos) {
                        std::string line = partial.substr(0, nl);
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        PushLine(line);
                        partial.erase(0, nl + 1);
                    }
                }
            }).detach();
        }

        // Raw x64 unwind. No heap use; guarded because the stack may be corrupt.
        int WalkStack(CONTEXT ctx, DWORD64* out, int max) {
            int n = 0;
            __try {
                while (n < max && ctx.Rip) {
                    out[n++] = ctx.Rip;
                    DWORD64 imageBase = 0;
                    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
                    if (!fn) {
                        ctx.Rip = *(DWORD64*)ctx.Rsp;
                        ctx.Rsp += 8;
                    } else {
                        PVOID handlerData = nullptr;
                        DWORD64 establisher = 0;
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx,
                                         &handlerData, &establisher, nullptr);
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            return n;
        }

        std::string FramesJson(const DWORD64* frames, int n) {
            std::string s = "[";
            for (int i = 0; i < n; i++) {
                if (i) s += ",";
                s += JsonString(FormatAddress(frames[i]));
            }
            return s + "]";
        }

        std::string TailJson() {
            auto lines = RecentLines();
            size_t start = lines.size() > 50 ? lines.size() - 50 : 0;
            std::string s = "[";
            for (size_t i = start; i < lines.size(); i++) {
                if (i > start) s += ",";
                s += JsonString(lines[i]);
            }
            return s + "]";
        }

        void WriteDump(const std::wstring& path, EXCEPTION_POINTERS* ep, DWORD threadId) {
            HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE) return;
            MINIDUMP_EXCEPTION_INFORMATION mei{ threadId, ep, FALSE };
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory |
                                MiniDumpWithUnloadedModules),
                ep ? &mei : nullptr, nullptr, nullptr);
            CloseHandle(f);
        }

        void WriteJson(const std::wstring& path, const std::string& body) {
            std::wstring tmp = path + L".tmp";
            { std::ofstream o(tmp, std::ios::binary); o << body; }
            MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
        }

        bool IsFatal(DWORD code) {
            switch (code) {
            case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_ILLEGAL_INSTRUCTION:
            case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_STACK_OVERFLOW:
            case EXCEPTION_PRIV_INSTRUCTION: case EXCEPTION_IN_PAGE_ERROR:
            case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: case 0xC0000374 /* heap corruption */:
            case 0xC0000409 /* stack buffer overrun */:
                return true;
            }
            return false;
        }

        void HandleCrash(const CrashRequest& r) {
            int index;
            {
                std::lock_guard lk(g_crashMutex);
                index = ++g_reports;
            }
            std::wstring base = g_runDir + L"\\" + g_instance + L"." + std::to_wstring(index);
            WriteDump(base + L".dmp", r.ep, r.threadId);
            DWORD64 frames[32];
            int n = WalkStack(*r.ep->ContextRecord, frames, 32);
            char code[16];
            std::snprintf(code, sizeof code, "0x%08lX", r.ep->ExceptionRecord->ExceptionCode);
            std::ostringstream j;
            j << "{\"code\":\"" << code << "\""
              << ",\"address\":" << JsonString(FormatAddress((DWORD64)r.ep->ExceptionRecord->ExceptionAddress))
              << ",\"frames\":" << FramesJson(frames, n)
              << ",\"thread\":" << r.threadId
              << ",\"game_thread\":" << (r.threadId == g_gameThreadId ? "true" : "false")
              << ",\"first_chance\":" << (r.firstChance ? "true" : "false")
              << ",\"dump\":" << JsonString(Narrow(g_instance) + "." + std::to_string(index) + ".dmp")
              << ",\"log_tail\":" << TailJson() << "}";
            WriteJson(base + L".crash.json", j.str());
        }

        void CrashWorker() {
            for (;;) {
                WaitForSingleObject(g_requestEvent, INFINITE);
                HandleCrash(g_request);
                SetEvent(g_doneEvent);
            }
        }

        // The dump is written from a separate thread: MiniDumpWriteDump is unreliable on the faulting thread.
        void Report(EXCEPTION_POINTERS* ep, bool firstChance) {
            static std::mutex oneAtATime;
            if (GetCurrentThreadId() == g_workerThreadId) return;
            std::lock_guard lk(oneAtATime);
            g_request = { ep, GetCurrentThreadId(), firstChance };
            SetEvent(g_requestEvent);
            WaitForSingleObject(g_doneEvent, 30000);
        }

        LONG CALLBACK VectoredHandler(EXCEPTION_POINTERS* ep) {
            if (!IsFatal(ep->ExceptionRecord->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
            {
                std::lock_guard lk(g_crashMutex);
                if (g_reports >= kMaxReports) return EXCEPTION_CONTINUE_SEARCH;
                if (!g_seenAddresses.insert((unsigned long long)ep->ExceptionRecord->ExceptionAddress).second)
                    return EXCEPTION_CONTINUE_SEARCH;
            }
            Report(ep, true);
            return EXCEPTION_CONTINUE_SEARCH;
        }

        LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* ep) {
            bool room;
            {
                std::lock_guard lk(g_crashMutex);
                room = g_reports < kMaxReports + 1;   // always room for the final, second-chance report
            }
            if (room) Report(ep, false);
            return g_previousFilter ? g_previousFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
        }

        void Watchdog() {
            for (;;) {
                Sleep(1000);
                if (g_ticks == 0 || MillisSinceLastTick() < g_hangSeconds * 1000LL) continue;
                DWORD64 frames[32];
                int n = 0;
                HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                       FALSE, g_gameThreadId);
                if (th) {
                    SuspendThread(th);
                    CONTEXT ctx{};
                    ctx.ContextFlags = CONTEXT_FULL;
                    if (GetThreadContext(th, &ctx)) n = WalkStack(ctx, frames, 32);
                    ResumeThread(th);
                    CloseHandle(th);
                }
                std::wstring base = g_runDir + L"\\" + g_instance + L".hang";
                WriteDump(base + L".dmp", nullptr, 0);
                std::ostringstream j;
                j << "{\"seconds\":" << g_hangSeconds << ",\"frames\":" << FramesJson(frames, n)
                  << ",\"dump\":" << JsonString(Narrow(g_instance) + ".hang.dmp")
                  << ",\"log_tail\":" << TailJson() << "}";
                WriteJson(base + L".json", j.str());
                std::printf("[DIAG] game thread frozen for %d s; wrote hang report\n", g_hangSeconds);
                return;   // one hang report per process
            }
        }

        std::wstring DefaultRunDir() {
            wchar_t* profile = nullptr;
            size_t len = 0;
            _wdupenv_s(&profile, &len, L"USERPROFILE");
            std::wstring root = profile ? profile : L".";
            free(profile);
            wchar_t stamp[32];
            std::time_t t = std::time(nullptr);
            std::tm tm{};
            localtime_s(&tm, &t);
            std::wcsftime(stamp, 32, L"%Y%m%d-%H%M%S", &tm);
            return root + L"\\Documents\\RebornLogs\\" + g_instance + L"-" + stamp;
        }
    }

    void Init(const std::wstring& runDir, const std::wstring& instance, int hangSeconds) {
        g_instance = instance;
        g_hangSeconds = hangSeconds;
        g_runDir = runDir.empty() ? DefaultRunDir() : runDir;
        std::error_code ec;
        std::filesystem::create_directories(g_runDir, ec);
        StartLogTee();
        g_requestEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_doneEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        std::thread worker(CrashWorker);
        g_workerThreadId = GetThreadId(worker.native_handle());
        worker.detach();
        AddVectoredExceptionHandler(0, VectoredHandler);   // 0 = run after other handlers
        g_previousFilter = SetUnhandledExceptionFilter(UnhandledFilter);
        std::thread(Watchdog).detach();
        std::printf("[DIAG] run dir %ls, instance %ls, freeze limit %d s\n",
                    g_runDir.c_str(), g_instance.c_str(), g_hangSeconds);
    }

    void NoteTick() {
        if (g_gameThreadId == 0) g_gameThreadId = GetCurrentThreadId();
        g_lastTickMs = NowMs();
        ++g_ticks;
    }

    long long MillisSinceLastTick() {
        long long last = g_lastTickMs;
        return last == 0 ? 0 : NowMs() - last;
    }

    unsigned long long TickCount() { return g_ticks; }

    std::vector<std::string> RecentLines() {
        std::lock_guard lk(g_linesMutex);
        return { g_lines.begin(), g_lines.end() };
    }

    std::string LastLineContaining(const std::string& needle) {
        std::lock_guard lk(g_linesMutex);
        for (auto it = g_lines.rbegin(); it != g_lines.rend(); ++it)
            if (it->find(needle) != std::string::npos) return *it;
        return {};
    }

    std::string FormatAddress(unsigned long long address) {
        HMODULE mod = nullptr;
        char buf[64];
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCWSTR)address, &mod) || !mod) {
            std::snprintf(buf, sizeof buf, "0x%llx", address);
            return buf;
        }
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(mod, path, MAX_PATH);
        std::string name = Narrow(std::filesystem::path(path).stem().wstring());
        for (auto& c : name) c = (char)tolower((unsigned char)c);
        std::snprintf(buf, sizeof buf, "+0x%llx", address - (unsigned long long)mod);
        return name + buf;
    }

    const std::wstring& RunDir() { return g_runDir; }
}
```

- [ ] **Step 5: Run**

Run: `python -m pytest debugloop/tests/test_native.py -v` → all PASS.
If `test_hang_writes_hang_report` fails only on the frame check, print `r["frames"]` and accept the actual top system module name (it may be `ntdll` or `kernelbase`); the frames list must be non-empty.

- [ ] **Step 6: Commit**

```bash
git add reborn/Diagnostics.hpp reborn/Diagnostics.cpp reborn/tests/crashme.cpp debugloop/tests/test_native.py
git commit -m "feat(reborn): crash dumps, crash reports, freeze watchdog, log tee"
```

---

### Task 7: C++ debug server and game state

**Files:**
- Create: `reborn/DebugServer.hpp`, `reborn/DebugServer.cpp`, `reborn/GameState.hpp`, `reborn/GameState.cpp`, `reborn/tests/test_debug_server.cpp`
- Modify: `debugloop/tests/test_native.py`

**Interfaces:**
- Consumes: `Diagnostics::MillisSinceLastTick`, `Diagnostics::RecentLines`, `Diagnostics::LastLineContaining`, `LaunchOptions::Get`.
- Produces (C++):
  - `DebugServer::Start(int port, StateFn, ExecFn) -> bool`, `DebugServer::Pump()` (game thread, each tick), `DebugServer::Stop()`; `StateFn = std::function<std::string()>`, `ExecFn = std::function<std::string(const std::string&)>`.
  - `GameState::SnapshotJson() -> std::string`, `GameState::Exec(const std::string&) -> std::string`, `GameState::SetListening(bool)`.
- HTTP (127.0.0.1 only): `GET /ping` → `pong` (no game thread). `GET /state` → snapshot JSON, or 503 `{"error":"game_thread_unresponsive","ms_since_tick":N}` after 3 s. `POST /exec` body = console command → `ok`. `GET /log` → recent console lines.
- `/state` JSON (all roles): `instance, role ("server"|"client"|"solo"), pid, ticks, map, net_mode, memory_mb, match_over`. Server adds: `listening, connections, network_objects, player_locations {name: [x,y,z]}`. Client/solo adds: `has_pawn, pawn_location [x,y,z] | null, pawn_health, connected, disconnect_reason, autopilot`.

- [ ] **Step 1: Write the failing native test `reborn/tests/test_debug_server.cpp`**

```cpp
#include "../httplib.h"
#include "../DebugServer.hpp"
#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <thread>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

int main() {
    std::atomic<bool> pumping{ true }, quit{ false };
    CHECK(DebugServer::Start(18999,
        [] { return std::string(R"({"ok":true})"); },
        [](const std::string& c) { return "ran " + c; }));
    std::thread game([&] {
        while (!quit) { if (pumping) DebugServer::Pump(); Sleep(10); }
    });

    httplib::Client cli("127.0.0.1", 18999);
    cli.set_read_timeout(10, 0);
    auto r = cli.Get("/ping");
    CHECK(r && r->body == "pong");
    r = cli.Get("/state");
    CHECK(r && r->status == 200 && r->body == R"({"ok":true})");
    r = cli.Post("/exec", "stat fps", "text/plain");
    CHECK(r && r->body == "ran stat fps");

    pumping = false;
    ULONGLONG t0 = GetTickCount64();
    r = cli.Get("/state");
    ULONGLONG dt = GetTickCount64() - t0;
    CHECK(r && r->status == 503);
    CHECK(r && r->body.find("game_thread_unresponsive") != std::string::npos);
    CHECK(dt < 5000);

    quit = true;
    game.join();
    DebugServer::Stop();
    std::puts(failures ? "FAIL" : "PASS");
    return failures;
}
```

Add to `debugloop/tests/test_native.py`:
```python
def test_debug_server():
    p = build_and_run("test_debug_server",
                      [SRC / "tests" / "test_debug_server.cpp", SRC / "DebugServer.cpp",
                       SRC / "Diagnostics.cpp"])
    assert p.returncode == 0, p.stdout + p.stderr
    assert "PASS" in p.stdout
```

Run: `python -m pytest debugloop/tests/test_native.py::test_debug_server -v` → FAIL (missing source).

- [ ] **Step 2: Write `reborn/DebugServer.hpp`**

```cpp
#pragma once
#include <functional>
#include <string>

namespace DebugServer {
    using StateFn = std::function<std::string()>;
    using ExecFn = std::function<std::string(const std::string&)>;

    bool Start(int port, StateFn state, ExecFn exec);
    void Pump();   // game thread, once per tick
    void Stop();
}
```

- [ ] **Step 3: Write `reborn/DebugServer.cpp`**

```cpp
#include "httplib.h"   // must come before anything that includes Windows.h
#include "DebugServer.hpp"
#include "Diagnostics.hpp"
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

namespace DebugServer {
    namespace {
        std::unique_ptr<httplib::Server> g_server;
        std::thread g_thread;
        std::mutex g_mutex;
        std::queue<std::packaged_task<std::string()>> g_queue;
        StateFn g_state;
        ExecFn g_exec;
        constexpr auto kWait = std::chrono::seconds(3);

        // Game objects may only be touched on the game thread: queue the work and wait for Pump().
        bool RunOnGameThread(std::function<std::string()> fn, std::string& out) {
            std::packaged_task<std::string()> task(std::move(fn));
            auto result = task.get_future();
            {
                std::lock_guard lk(g_mutex);
                g_queue.push(std::move(task));
            }
            if (result.wait_for(kWait) != std::future_status::ready) return false;
            out = result.get();
            return true;
        }

        void Unresponsive(httplib::Response& res) {
            res.status = 503;
            res.set_content(R"({"error":"game_thread_unresponsive","ms_since_tick":)" +
                            std::to_string(Diagnostics::MillisSinceLastTick()) + "}", "application/json");
        }

        void Answer(httplib::Response& res, std::function<std::string()> fn, const char* type) {
            std::string out;
            try {
                if (!RunOnGameThread(std::move(fn), out)) return Unresponsive(res);
            }
            catch (const std::exception& e) {
                res.status = 500;
                res.set_content(e.what(), "text/plain");
                return;
            }
            res.set_content(out, type);
        }
    }

    bool Start(int port, StateFn state, ExecFn exec) {
        g_state = std::move(state);
        g_exec = std::move(exec);
        g_server = std::make_unique<httplib::Server>();
        g_server->Get("/ping", [](const httplib::Request&, httplib::Response& res) {
            res.set_content("pong", "text/plain");
        });
        g_server->Get("/state", [](const httplib::Request&, httplib::Response& res) {
            Answer(res, g_state, "application/json");
        });
        g_server->Post("/exec", [](const httplib::Request& req, httplib::Response& res) {
            std::string cmd = req.body;
            Answer(res, [cmd] { return g_exec(cmd); }, "text/plain");
        });
        g_server->Get("/log", [](const httplib::Request&, httplib::Response& res) {
            std::string s;
            for (auto& line : Diagnostics::RecentLines()) { s += line; s += '\n'; }
            res.set_content(s, "text/plain");
        });
        if (!g_server->bind_to_port("127.0.0.1", port)) {
            std::printf("[DEBUG] could not bind 127.0.0.1:%d\n", port);
            return false;
        }
        g_thread = std::thread([] { g_server->listen_after_bind(); });
        std::printf("[DEBUG] debug server on 127.0.0.1:%d\n", port);
        return true;
    }

    void Pump() {
        for (int i = 0; i < 8; i++) {
            std::packaged_task<std::string()> task;
            {
                std::lock_guard lk(g_mutex);
                if (g_queue.empty()) return;
                task = std::move(g_queue.front());
                g_queue.pop();
            }
            task();
        }
    }

    void Stop() {
        if (g_server) g_server->stop();
        if (g_thread.joinable()) g_thread.join();
        g_server.reset();
    }
}
```

- [ ] **Step 4: Run the native test**

Run: `python -m pytest debugloop/tests/test_native.py::test_debug_server -v` → PASS.

- [ ] **Step 5: Write `reborn/GameState.hpp`**

```cpp
#pragma once
#include <string>

namespace GameState {
    std::string SnapshotJson();
    std::string Exec(const std::string& command);
    void SetListening(bool listening);
}
```

- [ ] **Step 6: Write `reborn/GameState.cpp`**

SDK names used here: `AController::Pawn`, `AActor::Location` (`FVector X/Y/Z`), `APawn::Health`, `AController::PlayerReplicationInfo`, `APlayerReplicationInfo::PlayerName`, `APlayerController::Player`. Before compiling, confirm each with Grep in `reborn/BB/SDK_HEADERS/Engine_classes.hpp` (e.g. pattern `class APawn\b` then `Health`). For converting the `FString PlayerName` to `std::string`, use whatever helper the existing code uses — Grep `PlayerName` in `reborn/*.cpp` and copy that pattern into `ToStd` below.

```cpp
#include "GameState.hpp"
#include "Autopilot.hpp"
#include "Diagnostics.hpp"
#include "Engine.hpp"
#include "Globals.hpp"
#include "LaunchOptions.hpp"
#include "Utils.hpp"
#include "json.hpp"
#include <Windows.h>
#include <Psapi.h>
#include <atomic>
#pragma comment(lib, "psapi.lib")

namespace GameState {
    namespace {
        std::atomic<bool> g_listening{ false };

        std::string MapName() {
            UWorld* world = Globals::GetGWorld();
            if (!world) return "";
            std::string full = world->GetFullName();          // e.g. "World Dojo_P.TheWorld"
            size_t sp = full.find(' '), dot = full.find('.');
            if (sp == std::string::npos || dot == std::string::npos || dot <= sp) return full;
            return full.substr(sp + 1, dot - sp - 1);
        }

        bool IsDefault(UObject* o) { return o->GetFullName().find("Default__") != std::string::npos; }

        nlohmann::json Loc(AActor* a) { return { a->Location.X, a->Location.Y, a->Location.Z }; }

        std::string ToStd(const FString& s) { return s.ToString(); }   // replace with the codebase's helper if different

        std::string DisconnectReason() {
            std::string line = Diagnostics::LastLineContaining("Failure");
            if (line.empty()) return "";
            if (line.find("Timeout") != std::string::npos || line.find("timed out") != std::string::npos) return "timeout";
            if (line.find("ConnectionLost") != std::string::npos) return "lost";
            if (line.find("PendingConnectionFailure") != std::string::npos) return "pending";
            return "other";
        }
    }

    void SetListening(bool listening) { g_listening = listening; }

    std::string SnapshotJson() {
        const auto& opt = LaunchOptions::Get();
        nlohmann::json j;
        std::string role = Globals::amServer ? "server" : (Globals::amStandalone ? "solo" : "client");
        std::string map = MapName();
        PROCESS_MEMORY_COUNTERS pmc{};
        GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
        j["instance"] = std::string(opt.instance.begin(), opt.instance.end());
        j["role"] = role;
        j["pid"] = GetCurrentProcessId();
        j["ticks"] = Diagnostics::TickCount();
        j["map"] = map;
        j["net_mode"] = role == "server" ? "dedicated" : (role == "solo" ? "standalone" : "client");
        j["memory_mb"] = pmc.WorkingSetSize / (1024 * 1024);
        j["match_over"] = !Diagnostics::LastLineContaining("Match ended").empty();

        if (Globals::amServer) {
            nlohmann::json locs = nlohmann::json::object();
            int connections = 0;
            for (APoplarPlayerController* pc : SDKUtils::GetAllOfClass<APoplarPlayerController>()) {
                if (!pc || IsDefault(pc) || !pc->Player) continue;
                connections++;
                if (pc->Pawn && pc->PlayerReplicationInfo)
                    locs[ToStd(pc->PlayerReplicationInfo->PlayerName)] = Loc(pc->Pawn);
            }
            j["listening"] = g_listening.load();
            j["connections"] = connections;
            j["network_objects"] = SDKUtils::GetAllOfClass<UActorChannel>().size();
            j["player_locations"] = locs;
        } else {
            APoplarPlayerController* pc = SDKUtils::GetLastOfClass<APoplarPlayerController>();
            bool hasPawn = pc && !IsDefault(pc) && pc->Pawn;
            j["has_pawn"] = hasPawn;
            j["pawn_location"] = hasPawn ? Loc(pc->Pawn) : nlohmann::json(nullptr);
            j["pawn_health"] = hasPawn ? pc->Pawn->Health : 0;
            bool inMenu = map.empty() || map.find("MenuMap") != std::string::npos;
            j["connected"] = role == "client" ? !inMenu : true;
            j["disconnect_reason"] = DisconnectReason();
            j["autopilot"] = Autopilot::PhaseName();
        }
        return j.dump();
    }

    std::string Exec(const std::string& command) {
        std::wstring w(command.begin(), command.end());
        Engine::ExecConsoleCommand(w.c_str());
        return "ok";
    }
}
```

`GameState.cpp` is compiled only inside the mod (it needs the SDK), so it is verified by the mod build in Task 9 and live in Task 14.

- [ ] **Step 7: Commit**

```bash
git add reborn/DebugServer.hpp reborn/DebugServer.cpp reborn/GameState.hpp reborn/GameState.cpp reborn/tests/test_debug_server.cpp debugloop/tests/test_native.py
git commit -m "feat(reborn): localhost debug server and game state snapshot"
```

---

### Task 8: C++ autopilot

**Files:**
- Create: `reborn/Autopilot.hpp`, `reborn/Autopilot.cpp`
- Modify: `reborn/Overlay.cpp:336-353` and `reborn/Overlay.hpp` (extract `LockInCharacter`)

**Interfaces:**
- Consumes: `LaunchOptions::Get()`, `Overlay::StartLaunchSequence(const wchar_t*)`, `Engine::ExecConsoleCommand(const wchar_t*)`, `Globals::selectedCharacter`, `Globals::amStandalone`, `Globals::LaunchSequenceState`, `Globals::CharacterSelectHasLockedIn`, `Globals::saveFiles`, `Globals::CurrentSaveFile`, `Metagame::ReadAllSaves()`, `Metagame::CreateNewSave(std::string, bool)`, `Metagame::ReverseCharacterLookup`, `Constants::CharacterSelectCharacterTable` (30 entries).
- Produces:
  - `Autopilot::Active() -> bool` (flag set and not server)
  - `Autopilot::OnMainMenuReady()` (call from `StartupCompletedHook`)
  - `Autopilot::Tick(float dt)` (game thread, after the engine tick)
  - `Autopilot::BeforePlayerTick(UObject* controller)` (from `ProcessEventHook` when the function is the player controller's `PlayerTick`)
  - `Autopilot::PhaseName() -> const char*` — `"off"`, `"waiting_for_menu"`, `"menu_ready"`, `"launching"`, `"character_select"`, `"playing"`
  - `Overlay::LockInCharacter(int index)`

How it plays: always walk forward; every 2–4 s pick a new turn rate and strafe; jump every 3–6 s; hold fire 1 s out of every 3. If the pawn moves less than 100 units in 5 s, turn hard for 1 s and jump. With no pawn while playing (dead), press fire every 5 s to respawn. All randomness comes from `std::mt19937(seed)`.

How it gets into a game:
- Solo (`-rbsolomap`): make sure a save exists, set `selectedCharacter`, set `amStandalone = true`, set `LaunchCommand = "open <map>"` and run it directly (this is what the overlay's Start! button does at `Overlay.cpp:305-308`, minus the gear screens).
- Client (`-rbjoin`): `StartLaunchSequence("open <ip:port>")` (same as Direct Connect). When the server's character select appears (in a non-menu map, not locked in, 5 s after arriving), call `Overlay::LockInCharacter(index)`.

- [ ] **Step 1: Extract `LockInCharacter` in `reborn/Overlay.cpp`**

Replace the body of the Lock In button (lines 337–353) with a call, and move the code into a new function above `Render()`:

```cpp
    void LockInCharacter(int i) {
        Globals::CharacterSelectHasLockedIn = true;

        // We're on the client here, so we should only ever have one PPC (aside from the CDO)
        APoplarPlayerController* ppc = SDKUtils::GetLastOfClass<APoplarPlayerController>();

        ppc->ServerCharacterSelectInput(i);

        ppc->ServerSetHasReceivedEntitlements();
        ppc->eventServerSelectCharacter(nullptr, nullptr, nullptr, true);
        ppc->ServerPlayerSelectClass(L"", L"");

        ppc->ServerCharacterSelectInput(i);

        Globals::selectedCharacter = Metagame::ReverseCharacterLookup(Constants::CharacterSelectCharacterTable[i]);

        std::thread t(UnfuckCharacterSelect, ppc, i);
        t.detach();
    }
```

Button becomes:
```cpp
                            if (ImGui::Button(("Lock In " + Constants::CharacterSelectCharacterTable[i]).c_str())) {
                                LockInCharacter(i);
                            }
```

Declare `void LockInCharacter(int i);` in `reborn/Overlay.hpp` inside `namespace Overlay`.

- [ ] **Step 2: Write `reborn/Autopilot.hpp`**

```cpp
#pragma once

class UObject;

namespace Autopilot {
    bool Active();
    void OnMainMenuReady();
    void Tick(float dt);
    void BeforePlayerTick(UObject* controller);
    const char* PhaseName();
}
```

If `class UObject;` conflicts with the SDK's declaration, include the SDK header the other `.hpp` files use instead (Grep `#include` in `Hooks.hpp`).

- [ ] **Step 3: Write `reborn/Autopilot.cpp`**

UPlayerInput axis field names: Grep `aBaseY` and `aStrafe` and `aTurn` in `reborn/BB/SDK_HEADERS/Engine_classes.hpp` and use the exact names found (UE3 standard is `aBaseY`, `aStrafe`, `aTurn`, `aLookUp`).

```cpp
#include "Autopilot.hpp"
#include "Constants.hpp"
#include "Engine.hpp"
#include "Globals.hpp"
#include "LaunchOptions.hpp"
#include "Metagame.hpp"
#include "Overlay.hpp"
#include "Utils.hpp"
#include <Windows.h>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>

namespace Autopilot {
    namespace {
        enum class Phase { Off, WaitingForMenu, MenuReady, Launching, CharacterSelect, Playing };
        Phase g_phase = Phase::WaitingForMenu;
        float g_phaseTime = 0.f;          // seconds in the current phase
        std::mt19937 g_rng;
        bool g_seeded = false;

        // Movement plan, refreshed on timers
        float g_turn = 0.f, g_strafe = 0.f;
        float g_untilNewPlan = 0.f, g_untilJump = 0.f, g_fireCycle = 0.f;
        float g_stuckTimer = 0.f, g_unstickFor = 0.f;
        float g_lastX = 0.f, g_lastY = 0.f;
        bool g_firing = false;
        float g_deadTimer = 0.f;
        bool g_testFired = false;

        float Rand(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(g_rng); }

        void SetPhase(Phase p) {
            g_phase = p;
            g_phaseTime = 0.f;
            std::printf("[AUTOPILOT] phase %s\n", PhaseName());
        }

        void Exec(const wchar_t* cmd) { Engine::ExecConsoleCommand(cmd); }

        bool InMenu() {
            UWorld* w = Globals::GetGWorld();
            return !w || w->GetFullName().find("MenuMap") != std::string::npos;
        }

        APoplarPlayerController* LocalPC() {
            APoplarPlayerController* pc = SDKUtils::GetLastOfClass<APoplarPlayerController>();
            if (!pc || pc->GetFullName().find("Default__") != std::string::npos) return nullptr;
            return pc;
        }

        int CharacterIndex(const std::string& name) {
            for (int i = 0; i < 30; i++) {
                const std::string& entry = Constants::CharacterSelectCharacterTable[i];
                if (entry == name || Metagame::ReverseCharacterLookup(entry) == name) return i;
            }
            return 0;
        }

        void EnsureSaveLoaded() {
            if (Globals::saveFiles.empty()) Globals::saveFiles = Metagame::ReadAllSaves();
            if (Globals::saveFiles.empty()) {
                Metagame::CreateNewSave("autopilot", true);
                Globals::saveFiles = Metagame::ReadAllSaves();
            }
            if (Globals::CurrentSaveFile >= (int)Globals::saveFiles.size()) Globals::CurrentSaveFile = 0;
        }

        void ChooseCharacter() {
            const auto& opt = LaunchOptions::Get();
            if (!opt.character.empty()) {
                Globals::selectedCharacter = Metagame::ReverseCharacterLookup(
                    Constants::CharacterSelectCharacterTable[CharacterIndex(opt.character)]);
            } else if (!Globals::saveFiles.empty() && !Globals::saveFiles[Globals::CurrentSaveFile].characters.empty()) {
                Globals::selectedCharacter = Globals::saveFiles[Globals::CurrentSaveFile].characters[0].characterDisplayName;
            }
        }

        void Launch() {
            const auto& opt = LaunchOptions::Get();
            EnsureSaveLoaded();
            ChooseCharacter();
            if (!opt.soloMap.empty()) {
                Globals::amStandalone = true;
                Globals::GearSlotOne = Globals::GearSlotTwo = Globals::GearSlotThree = nullptr;
                Globals::CharacterSkin = nullptr;
                Globals::CharacterTaunt = nullptr;
                Globals::LaunchCommand = _wcsdup((L"open " + opt.soloMap).c_str());
                Globals::LaunchSequenceState = Globals::ELaunchSequenceState::NotOpen;
                Exec(Globals::LaunchCommand);
            } else if (!opt.join.empty()) {
                Overlay::StartLaunchSequence(_wcsdup((L"open " + opt.join).c_str()));
            }
            SetPhase(Phase::Launching);
        }

        void PlayTick(float dt, APoplarPlayerController* pc) {
            if (!pc->Pawn) {
                g_deadTimer += dt;
                if (g_deadTimer > 5.f) { g_deadTimer = 0.f; Exec(L"StartFire"); Exec(L"StopFire"); }
                return;
            }
            g_deadTimer = 0.f;

            g_untilNewPlan -= dt;
            if (g_untilNewPlan <= 0.f) {
                g_turn = Rand(-0.6f, 0.6f);
                g_strafe = Rand(-1.f, 1.f);
                g_untilNewPlan = Rand(2.f, 4.f);
            }
            g_untilJump -= dt;
            if (g_untilJump <= 0.f) { Exec(L"Jump"); g_untilJump = Rand(3.f, 6.f); }

            g_fireCycle += dt;
            bool wantFire = std::fmod(g_fireCycle, 3.f) < 1.f;
            if (wantFire != g_firing) { Exec(wantFire ? L"StartFire" : L"StopFire"); g_firing = wantFire; }

            g_stuckTimer += dt;
            if (g_stuckTimer >= 5.f) {
                float dx = pc->Pawn->Location.X - g_lastX, dy = pc->Pawn->Location.Y - g_lastY;
                if (std::sqrt(dx * dx + dy * dy) < 100.f) { g_unstickFor = 1.f; Exec(L"Jump"); }
                g_lastX = pc->Pawn->Location.X;
                g_lastY = pc->Pawn->Location.Y;
                g_stuckTimer = 0.f;
            }
            if (g_unstickFor > 0.f) g_unstickFor -= dt;
        }
    }

    bool Active() { return LaunchOptions::Get().autopilot && !Globals::amServer; }

    const char* PhaseName() {
        if (!Active()) return "off";
        switch (g_phase) {
        case Phase::WaitingForMenu: return "waiting_for_menu";
        case Phase::MenuReady: return "menu_ready";
        case Phase::Launching: return "launching";
        case Phase::CharacterSelect: return "character_select";
        case Phase::Playing: return "playing";
        default: return "off";
        }
    }

    void OnMainMenuReady() {
        if (!Active() || g_phase != Phase::WaitingForMenu) return;
        SetPhase(Phase::MenuReady);
    }

    void Tick(float dt) {
        if (!Active()) return;
        const auto& opt = LaunchOptions::Get();
        if (!g_seeded) { g_rng.seed(opt.seed); g_seeded = true; }
        g_phaseTime += dt;
        APoplarPlayerController* pc = LocalPC();

        switch (g_phase) {
        case Phase::WaitingForMenu:
            break;
        case Phase::MenuReady:
            if (g_phaseTime > 5.f) Launch();
            break;
        case Phase::Launching:
            if (!InMenu() && pc) {
                if (!opt.join.empty() && !Globals::CharacterSelectHasLockedIn) SetPhase(Phase::CharacterSelect);
                else if (pc->Pawn) SetPhase(Phase::Playing);
            }
            break;
        case Phase::CharacterSelect:
            if (g_phaseTime > 5.f && !Globals::CharacterSelectHasLockedIn) {
                Overlay::LockInCharacter(CharacterIndex(opt.character));
            }
            if (pc && pc->Pawn && g_phaseTime > 5.f) SetPhase(Phase::Playing);
            break;
        case Phase::Playing:
            if (InMenu()) { SetPhase(Phase::MenuReady); break; }   // kicked back to the menu: try again
            if (pc) PlayTick(dt, pc);
            if (!g_testFired && g_phaseTime > 20.f) {
                if (opt.testCrash) { g_testFired = true; std::printf("[AUTOPILOT] -rbtestcrash\n"); *(volatile int*)nullptr = 1; }
                if (opt.testHang) { g_testFired = true; std::printf("[AUTOPILOT] -rbtesthang\n"); Sleep(INFINITE); }
            }
            break;
        default:
            break;
        }
    }

    void BeforePlayerTick(UObject* controller) {
        if (!Active() || g_phase != Phase::Playing) return;
        auto* pc = reinterpret_cast<APoplarPlayerController*>(controller);
        if (pc != LocalPC() || !pc->PlayerInput || !pc->Pawn) return;
        UPlayerInput* in = pc->PlayerInput;
        in->aBaseY = 1.0f;
        in->aStrafe = g_strafe;
        in->aTurn = g_unstickFor > 0.f ? 1.0f : g_turn;
        in->aLookUp = 0.f;
    }
}
```

Note: the "kicked back to the menu" branch means an autopilot client keeps retrying after a disconnect; the runner still grades the disconnect from the samples it took while playing.

- [ ] **Step 4: Commit** (the build check is in Task 9)

```bash
git add reborn/Autopilot.hpp reborn/Autopilot.cpp reborn/Overlay.cpp reborn/Overlay.hpp
git commit -m "feat(reborn): autopilot that joins or starts solo and plays"
```

---

### Task 9: Wire the modules into the mod, and deploy

**Files:**
- Modify: `reborn/dllmain.cpp:17-20`, `reborn/Init.cpp:158-` (`ServerConfig`), `reborn/Hooks.cpp:90-91` (tick), `reborn/Hooks.cpp:303-313` (`StartupCompletedHook`), `reborn/Hooks.cpp:394` (`ProcessEventHook`), `reborn/Hooks.cpp:844` (after `InitListen`), `reborn/reborn.vcxproj`
- Create: `debugloop/deploy.py`, `debugloop/tests/test_deploy.py`

**Interfaces:**
- Consumes: everything from Tasks 5–8; `build.build_mod()`.
- Produces: `deploy.deploy(dll: Path = config.MOD_DLL, win64: Path = config.WIN64) -> DeployResult(changed: list[str], backups: list[Path])`; `deploy.ensure_serverborn(win64) -> Path`.

- [ ] **Step 1: Hook the modules in**

`reborn/dllmain.cpp`, in `MainThread()` right after `Init::Console();`:
```cpp
    LaunchOptions::InitFromCommandLine();
    {
        const auto& opt = LaunchOptions::Get();
        Diagnostics::Init(opt.runDir, opt.instance, opt.hangSeconds);
        for (auto& e : opt.errors) std::printf("[OPTIONS] bad value: %ls\n", e.c_str());
        for (auto& u : opt.unknown) std::printf("[OPTIONS] unknown flag: %ls\n", u.c_str());
        if (opt.debugPort) DebugServer::Start(opt.debugPort, GameState::SnapshotJson, GameState::Exec);
    }
```
Add includes `LaunchOptions.hpp`, `Diagnostics.hpp`, `DebugServer.hpp`, `GameState.hpp` (after the existing `#include "httplib.h"`).

`reborn/Hooks.cpp`, `GameEngineTickHook`, right after `GameEngineTick.call<void>(engine, DeltaTime);`:
```cpp
        Diagnostics::NoteTick();
        DebugServer::Pump();
        Autopilot::Tick(DeltaTime);
```

`StartupCompletedHook`, after `ContinueToMenu();`:
```cpp
        Autopilot::OnMainMenuReady();
```

`ProcessEventHook`, first lines of the body:
```cpp
        if (Autopilot::Active()) {
            static UFunction* playerTick = nullptr;
            if (!playerTick && function->GetFullName().ends_with("PlayerController.PlayerTick")) playerTick = function;
            if (function == playerTick) Autopilot::BeforePlayerTick(object);
        }
```

`PoplarGameInfoSetupHook`, right after `ServerNetworking::InitListen();`:
```cpp
            GameState::SetListening(true);
```
Add includes `Diagnostics.hpp`, `DebugServer.hpp`, `Autopilot.hpp`, `GameState.hpp` to `Hooks.cpp`.

`reborn/Init.cpp`, `ServerConfig()`: after the `if (ServerSettings::amRunningWithGameCoordinator) { ... }` block, add:
```cpp
        {
            const auto& opt = LaunchOptions::Get();
            if (!ServerSettings::amRunningWithGameCoordinator && !opt.serverMap.empty()) {
                ServerSettings::MapString = _wcsdup((L"open " + opt.serverMap).c_str());
                std::printf("[OPTIONS] server map %ls\n", ServerSettings::MapString);
            }
            if (opt.players > 0) ServerSettings::NumPlayersToStart = opt.players;
        }
```
Add `#include "LaunchOptions.hpp"`.

`reborn/reborn.vcxproj`: add to the `<ClCompile>` item group
```xml
    <ClCompile Include="LaunchOptions.cpp" />
    <ClCompile Include="Diagnostics.cpp" />
    <ClCompile Include="DebugServer.cpp" />
    <ClCompile Include="GameState.cpp" />
    <ClCompile Include="Autopilot.cpp" />
```
and to the `<ClInclude>` item group the five matching `.hpp` files. Do **not** add `tests/*.cpp`.

- [ ] **Step 2: Build the mod**

Run: `python -c "from debugloop import build; r=build.build_mod(); print(r.ok); print(r.output[-4000:])"`
Expected: `True`. Fix compile errors in the new files only (SDK field names, `FString` conversion, include order — `httplib.h` must precede `Windows.h` in any file that includes both).

- [ ] **Step 3: Write the failing test `debugloop/tests/test_deploy.py`**

```python
from debugloop import deploy


def test_deploy_backs_up_then_copies(tmp_path):
    win64 = tmp_path / "Win64"; win64.mkdir()
    (win64 / "reborn.dll").write_bytes(b"old")
    (win64 / "Battleborn.exe").write_bytes(b"exe")
    dll = tmp_path / "reborn.dll"; dll.write_bytes(b"new")
    r = deploy.deploy(dll, win64)
    assert (win64 / "reborn.dll").read_bytes() == b"new"
    assert r.changed == ["reborn.dll"]
    assert r.backups and r.backups[0].read_bytes() == b"old"
    assert (win64 / "Serverborn.exe").read_bytes() == b"exe"


def test_deploy_same_file_is_noop(tmp_path):
    win64 = tmp_path / "Win64"; win64.mkdir()
    (win64 / "reborn.dll").write_bytes(b"same")
    (win64 / "Battleborn.exe").write_bytes(b"exe")
    dll = tmp_path / "reborn.dll"; dll.write_bytes(b"same")
    r = deploy.deploy(dll, win64)
    assert r.changed == [] and r.backups == []


def test_serverborn_refreshed_when_exe_changes(tmp_path):
    win64 = tmp_path / "Win64"; win64.mkdir()
    (win64 / "Battleborn.exe").write_bytes(b"v2")
    (win64 / "Serverborn.exe").write_bytes(b"v1")
    deploy.ensure_serverborn(win64)
    assert (win64 / "Serverborn.exe").read_bytes() == b"v2"
```

Run: `python -m pytest debugloop/tests/test_deploy.py -v` → FAIL.

- [ ] **Step 4: Write `debugloop/deploy.py`**

```python
import hashlib
import shutil
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path

from . import config


@dataclass
class DeployResult:
    changed: list[str] = field(default_factory=list)
    backups: list[Path] = field(default_factory=list)


def _sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest() if p.exists() else ""


def _backup(p: Path, win64: Path) -> Path:
    d = win64 / "rb_backups"
    d.mkdir(exist_ok=True)
    b = d / f"{p.name}.{datetime.now():%Y%m%d-%H%M%S}"
    shutil.copy2(p, b)
    return b


def ensure_serverborn(win64: Path = config.WIN64) -> Path:
    exe, server = win64 / "Battleborn.exe", win64 / "Serverborn.exe"
    if _sha(exe) != _sha(server):
        shutil.copy2(exe, server)
    return server


def deploy(dll: Path = config.MOD_DLL, win64: Path = config.WIN64) -> DeployResult:
    r = DeployResult()
    target = win64 / "reborn.dll"
    if _sha(dll) != _sha(target):
        if target.exists():
            r.backups.append(_backup(target, win64))
        shutil.copy2(dll, target)
        r.changed.append("reborn.dll")
    pdb = dll.with_suffix(".pdb")
    if pdb.exists() and _sha(pdb) != _sha(win64 / "reborn.pdb"):
        shutil.copy2(pdb, win64 / "reborn.pdb")
    ensure_serverborn(win64)
    return r
```

Run: `python -m pytest debugloop/tests/test_deploy.py -v` → PASS.

- [ ] **Step 5: Deploy for real and keep the old DLL safe**

Run: `python -c "from debugloop import deploy; print(deploy.deploy())"`
Expected: `changed=['reborn.dll']` and a backup path under `D:\SteamLibrary\steamapps\common\Battleborn\Binaries\Win64\rb_backups\`.

- [ ] **Step 6: Commit**

```bash
git add reborn/dllmain.cpp reborn/Init.cpp reborn/Hooks.cpp reborn/reborn.vcxproj debugloop/deploy.py debugloop/tests/test_deploy.py
git commit -m "feat: wire diagnostics, debug server and autopilot into the mod; deploy helper"
```

---

### Task 10: Launcher and scenario runner

**Files:**
- Create: `debugloop/launch.py`, `debugloop/run.py`, `debugloop/tests/fake_game.py`, `debugloop/tests/test_run.py`

**Interfaces:**
- Consumes: `config.*`, `scenario.Scenario/ProcessSpec/find_scenario`, `outcome.Sample/ProcessRecord/classify`, `signature.make`.
- Produces:
  - `launch.GAME_BASE_ARGS: list[str]`
  - `launch.ProcessHandle(pid)` with `.pid`, `.exit_code() -> int | None`, `.kill()`
  - `launch.Launcher` protocol: `start(name: str, role: str, args: list[str]) -> ProcessHandle`
  - `launch.RealLauncher(win64=config.WIN64)`; `launch.ensure_identity(name, exe_name, args, win64) -> Path`
  - `launch.find_game_processes() -> list[int]` (pids of `Battleborn.exe` / `Serverborn.exe`)
  - `run.HarnessError(Exception)`
  - `run.RunResult(run_id: str, scenario: str, outcome: Outcome, signature: str | None, run_dir: Path, elapsed_s: float)`
  - `run.run_scenario(scn, launcher=None, runs_dir=config.RUNS_DIR, poll_s=2.0) -> RunResult` (raises `HarnessError`)
  - CLI: `python -m debugloop.run <scenario-name> [--poll S]` → prints `result.json`; exit 0 pass, 1 failed (bug), 2 harness error.
- Run dir contents: `scenario.toml` (copy), `timeline.jsonl` (one `Sample` per line), `result.json`, plus the game's own `<instance>.log`, crash and hang files.
- `runs/active.json`: `{"run_id": ..., "pids": {"server": 123, ...}}` while a run is live.

Startup order: server first, wait until its `/state` says `listening` (within 240 s), then each client 5 s apart. Solo scenarios start their one process directly.

Per-process args (in this order): `GAME_BASE_ARGS + spec.args + -rbinstance=<name> -rbdebugport=<port> -rbrundir=<run dir>`, plus `-rbplayers=<client count>` for the server and `-rbjoin=127.0.0.1:7777` for clients. Ports: 18080 + index in the scenario.

Steam identity per process (spike S2 in Task 15 confirms this works): `Win64\rb_ids\<name>\` holds a copy of the loader and the gbe_fork steamclient DLLs, a `ColdClientLoader.ini` pointing `Exe` at `..\..\Battleborn.exe` (or `Serverborn.exe`) with the args, and `steam_settings\configs.user.ini` with a unique `account_name` (= instance name, which is also the in-game player name the server reports) and `account_steamid`.

- [ ] **Step 1: Write the fake game `debugloop/tests/fake_game.py`**

```python
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
                "pawn_location": [0, 0, 0]}

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
```

- [ ] **Step 2: Write the failing tests `debugloop/tests/test_run.py`**

```python
import json
import subprocess
import sys
from pathlib import Path

import pytest

from debugloop import launch, run, scenario

FAKE = Path(__file__).with_name("fake_game.py")


class FakeLauncher:
    def __init__(self, modes: dict[str, str] | None = None):
        self.modes = modes or {}
        self.started: list[launch.ProcessHandle] = []

    def start(self, name, role, args):
        p = subprocess.Popen([sys.executable, str(FAKE), *args, f"--role={role}",
                              f"--mode={self.modes.get(name, 'pass')}"])
        h = launch.ProcessHandle(p.pid)
        self.started.append(h)
        return h


def scn(limit=6, two=False):
    text = f'name="t"\nstep=1\ntime_limit_s={limit}\nexpect_map="Dojo_P"\n'
    text += '[[process]]\nname="server"\nrole="server"\nargs=[]\n' if two else ""
    text += f'[[process]]\nname="c1"\nrole="{"client" if two else "solo"}"\nargs=[]\n'
    return scenario.parse(text, Path("t.toml"))


@pytest.fixture(autouse=True)
def no_foreign_games(monkeypatch):
    monkeypatch.setattr(launch, "find_game_processes", lambda: [])


def test_pass(tmp_path):
    L = FakeLauncher()
    r = run.run_scenario(scn(), L, tmp_path, poll_s=0.5)
    assert r.outcome.kind == "pass", r.outcome
    assert (r.run_dir / "result.json").exists()
    assert (r.run_dir / "timeline.jsonl").read_text().strip()
    assert all(h.exit_code() is not None for h in L.started), "runner must stop what it started"
    assert not (tmp_path / "active.json").exists()


def test_crash_keeps_dump(tmp_path):
    r = run.run_scenario(scn(30), FakeLauncher({"c1": "crash"}), tmp_path, poll_s=0.5)
    assert r.outcome.kind == "crash"
    assert r.signature == "crash:0xc0000005:battleborn+0x1234"
    assert (r.run_dir / "c1.1.dmp").exists()


def test_hang_from_503(tmp_path):
    r = run.run_scenario(scn(30), FakeLauncher({"c1": "hang"}), tmp_path, poll_s=0.5)
    assert r.outcome.kind == "hang"


def test_server_then_client(tmp_path):
    r = run.run_scenario(scn(8, two=True), FakeLauncher(), tmp_path, poll_s=0.5)
    assert r.outcome.kind == "pass", r.outcome


def test_refuses_when_foreign_game_running(tmp_path, monkeypatch):
    monkeypatch.setattr(launch, "find_game_processes", lambda: [424242])
    with pytest.raises(run.HarnessError, match="already running"):
        run.run_scenario(scn(), FakeLauncher(), tmp_path, poll_s=0.5)


def test_reaps_stale_active_file(tmp_path):
    stale = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
    (tmp_path / "active.json").write_text(json.dumps({"run_id": "old", "pids": {"c1": stale.pid}}))
    run.run_scenario(scn(), FakeLauncher(), tmp_path, poll_s=0.5)
    assert stale.wait(timeout=10) is not None


def test_passing_run_deletes_dumps(tmp_path):
    r = run.run_scenario(scn(), FakeLauncher(), tmp_path, poll_s=0.5)
    (r.run_dir / "x.dmp").write_bytes(b"x")
    run.prune_dumps_for_pass(r)
    assert not list(r.run_dir.glob("*.dmp"))
```

Run: `python -m pytest debugloop/tests/test_run.py -v` → FAIL (import errors).

- [ ] **Step 3: Write `debugloop/launch.py`**

```python
import configparser
import ctypes
import hashlib
import shutil
import subprocess
import time
import zlib
from pathlib import Path
from typing import Protocol

import psutil

from . import config

GAME_BASE_ARGS = ["-windowed", "-ResX=960", "-ResY=540", "-nosound", "-nomoviestartup", "-NOSPLASH"]
GAME_EXES = ("Battleborn.exe", "Serverborn.exe")
STEAMID_BASE = 76561197960287930
LOADER_FILES = ("steamclient_loader_x64.exe", "steamclient64.dll", "steamclient.dll")

_k32 = ctypes.WinDLL("kernel32", use_last_error=True)
_k32.OpenProcess.restype = ctypes.c_void_p
PROCESS_TERMINATE, PROCESS_QUERY_LIMITED, SYNCHRONIZE = 0x0001, 0x1000, 0x100000
STILL_ACTIVE = 259


class ProcessHandle:
    """Works for processes we did not spawn directly (the loader spawns the game)."""

    def __init__(self, pid: int):
        self.pid = pid
        self._h = _k32.OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED | SYNCHRONIZE, False, pid)
        if not self._h:
            raise OSError(f"cannot open process {pid}")

    def exit_code(self) -> int | None:
        code = ctypes.c_ulong()
        if not _k32.GetExitCodeProcess(ctypes.c_void_p(self._h), ctypes.byref(code)):
            return None
        return None if code.value == STILL_ACTIVE else ctypes.c_int32(code.value).value

    def kill(self) -> None:
        if self.exit_code() is None:
            _k32.TerminateProcess(ctypes.c_void_p(self._h), 1)
            _k32.WaitForSingleObject(ctypes.c_void_p(self._h), 10000)


class Launcher(Protocol):
    def start(self, name: str, role: str, args: list[str]) -> ProcessHandle: ...


def _sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest() if p.exists() else ""


def ensure_identity(name: str, exe_name: str, args: list[str], win64: Path = config.WIN64) -> Path:
    d = win64 / "rb_ids" / name
    (d / "steam_settings").mkdir(parents=True, exist_ok=True)
    for f in LOADER_FILES:
        if (win64 / f).exists() and _sha(win64 / f) != _sha(d / f):
            shutil.copy2(win64 / f, d / f)
    if (win64 / "steam_settings").is_dir():
        shutil.copytree(win64 / "steam_settings", d / "steam_settings", dirs_exist_ok=True)

    ini = configparser.ConfigParser(interpolation=None)
    ini.optionxform = str
    if (win64 / "ColdClientLoader.ini").exists():
        ini.read(win64 / "ColdClientLoader.ini", encoding="utf-8")
    if "SteamClient" not in ini:
        ini["SteamClient"] = {}
    sc = ini["SteamClient"]
    sc["Exe"] = str(win64 / exe_name)
    sc["ExeRunDir"] = str(win64)
    sc["ExeCommandLine"] = subprocess.list2cmdline(args)
    sc.setdefault("AppId", "394230")
    sc["SteamClientDll"] = str(d / "steamclient.dll")
    sc["SteamClient64Dll"] = str(d / "steamclient64.dll")
    with open(d / "ColdClientLoader.ini", "w", encoding="utf-8") as f:
        ini.write(f)

    user = configparser.ConfigParser(interpolation=None)
    user.optionxform = str
    user["user::general"] = {"account_name": name,
                             "account_steamid": str(STEAMID_BASE + zlib.crc32(name.encode()) % 100000),
                             "language": "english"}
    user["user::saves"] = {"local_save_path": str(d / "saves")}
    with open(d / "steam_settings" / "configs.user.ini", "w", encoding="utf-8") as f:
        user.write(f)
    return d


def find_game_processes() -> list[int]:
    return [p.pid for p in psutil.process_iter(["name"]) if p.info["name"] in GAME_EXES]


class RealLauncher:
    def __init__(self, win64: Path = config.WIN64):
        self.win64 = win64

    def start(self, name: str, role: str, args: list[str]) -> ProcessHandle:
        exe = "Serverborn.exe" if role == "server" else "Battleborn.exe"
        d = ensure_identity(name, exe, args, self.win64)
        subprocess.Popen([str(d / "steamclient_loader_x64.exe")], cwd=d)
        marker = f"-rbinstance={name}"
        deadline = time.time() + 60
        while time.time() < deadline:
            for p in psutil.process_iter(["name", "cmdline"]):
                if p.info["name"] == exe and marker in (p.info["cmdline"] or []):
                    return ProcessHandle(p.pid)
            time.sleep(0.5)
        raise RuntimeError(f"{exe} for {name} did not start within 60 s")
```

- [ ] **Step 4: Write `debugloop/run.py`**

```python
import argparse
import json
import shutil
import sys
import time
import urllib.error
import urllib.request
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path

from . import config, launch, scenario, signature
from .outcome import STARTUP_TIMEOUT_S, Outcome, ProcessRecord, Sample, classify


class HarnessError(Exception):
    pass


@dataclass
class RunResult:
    run_id: str
    scenario: str
    outcome: Outcome
    signature: str | None
    run_dir: Path
    elapsed_s: float


def _get_state(port: int) -> tuple[dict | None, int | None]:
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/state", timeout=4) as r:
            return json.loads(r.read()), r.status
    except urllib.error.HTTPError as e:
        return None, e.code
    except (urllib.error.URLError, TimeoutError, ConnectionError, json.JSONDecodeError):
        return None, None


def _reap_stale(runs_dir: Path) -> None:
    f = runs_dir / "active.json"
    if not f.exists():
        return
    for pid in json.loads(f.read_text()).get("pids", {}).values():
        try:
            launch.ProcessHandle(pid).kill()
        except OSError:
            pass
    f.unlink()


def _preconditions(runs_dir: Path) -> None:
    runs_dir.mkdir(parents=True, exist_ok=True)
    _reap_stale(runs_dir)
    foreign = launch.find_game_processes()
    if foreign:
        raise HarnessError(f"a game process is already running (pids {foreign}); "
                           "close it first - the runner only stops games it started")
    free_mb = shutil.disk_usage(runs_dir).free // (1024 * 1024)
    if free_mb < config.MIN_FREE_MB:
        raise HarnessError(f"only {free_mb} MB free disk space")


def _args(spec, port: int, run_dir: Path, n_clients: int) -> list[str]:
    a = launch.GAME_BASE_ARGS + list(spec.args) + [
        f"-rbinstance={spec.name}", f"-rbdebugport={port}", f"-rbrundir={run_dir}"]
    if spec.role == "server":
        a.append(f"-rbplayers={n_clients}")
    if spec.role == "client":
        a.append(f"-rbjoin=127.0.0.1:{config.SERVER_PORT}")
    return a


def _records(scn, handles, run_dir: Path) -> list[ProcessRecord]:
    recs = []
    for spec in scn.processes:
        crashes = [json.loads(p.read_text()) for p in sorted(run_dir.glob(f"{spec.name}.*.crash.json"))]
        hang_f = run_dir / f"{spec.name}.hang.json"
        log_f = run_dir / f"{spec.name}.log"
        tail = log_f.read_text(errors="replace").splitlines()[-80:] if log_f.exists() else []
        h = handles.get(spec.name)
        recs.append(ProcessRecord(spec.name, spec.role, h.exit_code() if h else None, crashes,
                                  json.loads(hang_f.read_text()) if hang_f.exists() else None, tail))
    return recs


def run_scenario(scn, launcher=None, runs_dir: Path = config.RUNS_DIR, poll_s: float = 2.0) -> RunResult:
    launcher = launcher or launch.RealLauncher()
    _preconditions(runs_dir)
    run_id = f"{datetime.now():%Y%m%d-%H%M%S}-{scn.name}"
    run_dir = runs_dir / run_id
    run_dir.mkdir(parents=True)
    shutil.copy2(scn.path, run_dir / "scenario.toml") if scn.path.exists() else None
    ports = {p.name: config.FIRST_DEBUG_PORT + i for i, p in enumerate(scn.processes)}
    n_clients = sum(p.role == "client" for p in scn.processes)
    handles: dict[str, launch.ProcessHandle] = {}
    samples: list[Sample] = []
    active = runs_dir / "active.json"
    t0 = time.time()

    def save_active():
        active.write_text(json.dumps({"run_id": run_id, "pids": {n: h.pid for n, h in handles.items()}}))

    def poll():
        with open(run_dir / "timeline.jsonl", "a", encoding="utf-8") as tl:
            for name, port in ports.items():
                if name not in handles:
                    continue
                st, code = _get_state(port)
                s = Sample(round(time.time() - t0, 1), name, st, code)
                samples.append(s)
                tl.write(json.dumps(asdict(s)) + "\n")

    try:
        servers = [p for p in scn.processes if p.role == "server"]
        others = [p for p in scn.processes if p.role != "server"]
        for spec in servers:
            handles[spec.name] = launcher.start(spec.name, spec.role,
                                                _args(spec, ports[spec.name], run_dir, n_clients))
            save_active()
            while True:
                poll()
                last = [s for s in samples if s.name == spec.name][-1]
                if last.state and last.state.get("listening"):
                    break
                if handles[spec.name].exit_code() is not None or time.time() - t0 > STARTUP_TIMEOUT_S:
                    break
                time.sleep(poll_s)
        for i, spec in enumerate(others):
            if i:
                time.sleep(5 if poll_s >= 1 else 0.5)
            handles[spec.name] = launcher.start(spec.name, spec.role,
                                                _args(spec, ports[spec.name], run_dir, n_clients))
            save_active()

        while True:
            poll()
            elapsed = time.time() - t0
            server_log = run_dir / "server.log"
            match_ended = server_log.exists() and "Match ended" in server_log.read_text(errors="replace")
            o = classify(scn, samples, _records(scn, handles, run_dir), elapsed, match_ended)
            if o.kind != "running":
                break
            time.sleep(poll_s)
    except launch_errors() as e:
        raise HarnessError(str(e)) from e
    finally:
        for h in handles.values():
            h.kill()
        active.unlink(missing_ok=True)

    sig = signature.make(o, scn)
    result = RunResult(run_id, scn.name, o, sig, run_dir, round(time.time() - t0, 1))
    (run_dir / "result.json").write_text(json.dumps(
        {**asdict(result), "run_dir": str(run_dir)}, indent=2, default=str))
    if o.kind == "pass":
        prune_dumps_for_pass(result)
    return result


def launch_errors():
    return (RuntimeError, OSError)


def prune_dumps_for_pass(r: RunResult) -> None:
    for d in r.run_dir.glob("*.dmp"):
        d.unlink()


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="python -m debugloop.run")
    ap.add_argument("scenario")
    ap.add_argument("--poll", type=float, default=2.0)
    a = ap.parse_args(argv)
    try:
        r = run_scenario(scenario.find_scenario(a.scenario), poll_s=a.poll)
    except (HarnessError, scenario.ScenarioError) as e:
        print(f"HARNESS ERROR: {e}")
        return 2
    print((r.run_dir / "result.json").read_text())
    return 0 if r.outcome.kind == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 5: Run**

Run: `python -m pytest debugloop/tests/test_run.py -v` → all PASS (about 1 minute).

- [ ] **Step 6: Commit**

```bash
git add debugloop/launch.py debugloop/run.py debugloop/tests/fake_game.py debugloop/tests/test_run.py
git commit -m "feat(debugloop): launcher with per-instance identity and scenario runner"
```

---

### Task 11: Triage (cdb, Ghidra names, known hooks)

**Files:**
- Create: `debugloop/analyze.py`, `debugloop/ghidra/ExportFunctions.java`, `debugloop/tests/test_analyze.py`

**Interfaces:**
- Consumes: `run.RunResult`, `config.WIN64`, `config.REPO`.
- Produces:
  - `analyze.known_addresses(src_dir: Path = REPO/"reborn") -> dict[int, str]` — RVA → hook name, parsed from `baseAddress + 0x...), &Hooks::Name` in `*.cpp`.
  - `analyze.load_functions(path = PKG/"ghidra"/"out"/"battleborn_functions.json") -> list[tuple[int,int,str]]` (empty if missing).
  - `analyze.describe_frame(frame: str, funcs, hooks) -> str` — e.g. `battleborn+0x1474150 (in FUN_141474140, hooked by PoplarGameInfoSetupHook)`.
  - `analyze.find_cdb() -> Path | None` (env `BB_CDB`, else WinDbg Store install `amd64\cdb.exe`, else Windows Kits).
  - `analyze.run_cdb(dump: Path, timeout=240) -> str`
  - `analyze.write_triage(result: RunResult) -> Path` → `<run_dir>/triage.md`
  - CLI: `python -m debugloop.analyze <run_dir>`
  - `analyze.export_ghidra_functions() -> Path` (one-time, slow: runs Ghidra headless on `Battleborn.exe`)

- [ ] **Step 1: Write the failing tests `debugloop/tests/test_analyze.py`**

```python
import json
from pathlib import Path

from debugloop import analyze
from debugloop.outcome import Outcome
from debugloop.run import RunResult


def test_known_addresses_parses_hook_table(tmp_path):
    (tmp_path / "Init.cpp").write_text(
        "Hooks::PoplarGameInfoSetup = safetyhook::create_inline((void*)(Globals::baseAddress + 0x1474140), &Hooks::PoplarGameInfoSetupHook);\n")
    assert analyze.known_addresses(tmp_path) == {0x1474140: "PoplarGameInfoSetupHook"}


def test_known_addresses_real_repo_has_tick_hook():
    assert "GameEngineTickHook" in analyze.known_addresses().values()


def test_describe_frame():
    funcs = [(0x1474140, 0x1474400, "FUN_141474140")]
    hooks = {0x1474140: "PoplarGameInfoSetupHook"}
    d = analyze.describe_frame("battleborn+0x1474150", funcs, hooks)
    assert "FUN_141474140" in d and "PoplarGameInfoSetupHook" in d
    assert analyze.describe_frame("reborn+0x10", funcs, hooks) == "reborn+0x10"


def test_write_triage_without_tools(tmp_path, monkeypatch):
    monkeypatch.setattr(analyze, "find_cdb", lambda: None)
    (tmp_path / "c1.1.crash.json").write_text(json.dumps(
        {"code": "0xC0000005", "frames": ["battleborn+0x10"], "dump": "c1.1.dmp"}))
    (tmp_path / "c1.log").write_text("line a\nline b\n")
    (tmp_path / "timeline.jsonl").write_text('{"t": 1, "name": "c1", "state": {"ticks": 3}, "http_status": 200}\n')
    r = RunResult("rid", "s1", Outcome("crash", "", "c1", "playing", "battleborn+0x10", "0xC0000005"),
                  "crash:0xc0000005:battleborn+0x10", tmp_path, 12.0)
    text = analyze.write_triage(r).read_text()
    assert "crash:0xc0000005:battleborn+0x10" in text
    assert "line b" in text
    assert "cdb not found" in text
```

Run: `python -m pytest debugloop/tests/test_analyze.py -v` → FAIL.

- [ ] **Step 2: Write `debugloop/analyze.py`**

```python
import bisect
import json
import os
import re
import subprocess
import sys
from pathlib import Path

from . import config

HOOK_RE = re.compile(r"baseAddress\s*\+\s*(0x[0-9a-fA-F]+)\)\s*,\s*&?Hooks::(\w+)")
FUNCS_JSON = config.PKG / "ghidra" / "out" / "battleborn_functions.json"
GHIDRA_DIR = Path(os.environ["LOCALAPPDATA"]) / "Programs" / "Ghidra" / "ghidra_12.1.4_PUBLIC"


def known_addresses(src_dir: Path = config.REPO / "reborn") -> dict[int, str]:
    out = {}
    for p in src_dir.glob("*.cpp"):
        for m in HOOK_RE.finditer(p.read_text(encoding="utf-8", errors="replace")):
            out[int(m.group(1), 16)] = m.group(2)
    return out


def load_functions(path: Path = FUNCS_JSON) -> list[tuple[int, int, str]]:
    if not path.exists():
        return []
    return sorted((f["start"], f["end"], f["name"]) for f in json.loads(path.read_text()))


def describe_frame(frame: str, funcs, hooks) -> str:
    mod, _, off = frame.partition("+")
    if mod not in ("battleborn", "serverborn"):
        return frame
    rva = int(off, 16)
    parts = []
    i = bisect.bisect_right([f[0] for f in funcs], rva) - 1
    if i >= 0 and funcs[i][0] <= rva <= funcs[i][1]:
        parts.append(f"in {funcs[i][2]}")
        if funcs[i][0] in hooks:
            parts.append(f"hooked by {hooks[funcs[i][0]]}")
    elif rva in hooks:
        parts.append(f"hooked by {hooks[rva]}")
    return f"{frame} ({', '.join(parts)})" if parts else frame


def find_cdb() -> Path | None:
    if os.environ.get("BB_CDB") and Path(os.environ["BB_CDB"]).exists():
        return Path(os.environ["BB_CDB"])
    try:
        loc = subprocess.run(["powershell", "-NoProfile", "-Command",
                              "(Get-AppxPackage Microsoft.WinDbg).InstallLocation"],
                             capture_output=True, text=True, timeout=30).stdout.strip()
        if loc and (Path(loc) / "amd64" / "cdb.exe").exists():
            return Path(loc) / "amd64" / "cdb.exe"
    except (OSError, subprocess.TimeoutExpired):
        pass
    kit = Path(r"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe")
    return kit if kit.exists() else None


def run_cdb(dump: Path, timeout: int = 240) -> str:
    cdb = find_cdb()
    if not cdb:
        return "(cdb not found)"
    cache = Path(os.environ["LOCALAPPDATA"]) / "symcache"
    cmd = (f".symfix+ {cache}; .sympath+ {config.WIN64}; .reload; "
           "!analyze -v; .ecxr; kb 30; q")
    try:
        p = subprocess.run([str(cdb), "-z", str(dump), "-c", cmd], capture_output=True,
                           text=True, timeout=timeout, errors="replace")
        lines = p.stdout.splitlines()
        return "\n".join(lines[-300:])
    except subprocess.TimeoutExpired:
        return "(cdb timed out)"


def write_triage(result) -> Path:
    d = result.run_dir
    funcs, hooks = load_functions(), known_addresses()
    o = result.outcome
    md = [f"# Triage: {result.signature}", "",
          f"- Scenario: `{result.scenario}`  run: `{result.run_id}`",
          f"- Outcome: **{o.kind}** {o.detail} (process `{o.process}`, phase `{o.phase}`, "
          f"after {result.elapsed_s} s)", ""]
    for rep in sorted(d.glob("*.crash.json")) + sorted(d.glob("*.hang.json")):
        r = json.loads(rep.read_text())
        md += [f"## {rep.name}", "", f"code `{r.get('code', 'hang')}` "
               f"first_chance `{r.get('first_chance')}`", "", "Frames:", ""]
        md += [f"{i}. `{describe_frame(f, funcs, hooks)}`" for i, f in enumerate(r.get("frames", []))]
        dump = d / r.get("dump", "")
        md += ["", "cdb:", "", "```",
               run_cdb(dump) if dump.is_file() else "(no dump)",
               "```", ""]
    if "cdb not found" not in "\n".join(md) and find_cdb() is None:
        md.append("(cdb not found)")
    for log in sorted(d.glob("*.log")):
        md += [f"## Last lines of {log.name}", "", "```",
               *log.read_text(errors="replace").splitlines()[-60:], "```", ""]
    tl = d / "timeline.jsonl"
    if tl.exists():
        md += ["## Last samples", "", "```", *tl.read_text().splitlines()[-12:], "```", ""]
    md += ["## Known hook addresses", "",
           *[f"- `battleborn+{hex(a)}` {n}" for a, n in sorted(hooks.items())]]
    out = d / "triage.md"
    out.write_text("\n".join(md), encoding="utf-8")
    return out


def export_ghidra_functions() -> Path:
    """One-time: Ghidra headless analysis of Battleborn.exe (can take an hour or more)."""
    proj = config.PKG / "ghidra" / "out" / "project"
    proj.mkdir(parents=True, exist_ok=True)
    cmd = [str(GHIDRA_DIR / "support" / "analyzeHeadless.bat"), str(proj), "bb",
           "-import", str(config.WIN64 / "Battleborn.exe"), "-overwrite",
           "-scriptPath", str(config.PKG / "ghidra"),
           "-postScript", "ExportFunctions.java", str(FUNCS_JSON), "-max-cpu", "4"]
    env = dict(os.environ, JAVA_HOME=r"C:\Program Files\Eclipse Adoptium\jdk-21.0.12.101-hotspot")
    subprocess.run(cmd, check=True, env=env)
    return FUNCS_JSON


if __name__ == "__main__":
    from .run import RunResult
    from .outcome import Outcome
    d = Path(sys.argv[1])
    res = json.loads((d / "result.json").read_text())
    r = RunResult(res["run_id"], res["scenario"], Outcome(**res["outcome"]), res["signature"],
                  d, res["elapsed_s"])
    print(write_triage(r))
```

- [ ] **Step 3: Write `debugloop/ghidra/ExportFunctions.java`**

```java
// Writes every function as {"name","start","end"} with addresses relative to the image base.
// @category Export
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import java.io.FileWriter;
import java.io.PrintWriter;

public class ExportFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        String out = getScriptArgs().length > 0 ? getScriptArgs()[0] : "functions.json";
        long base = currentProgram.getImageBase().getOffset();
        try (PrintWriter w = new PrintWriter(new FileWriter(out))) {
            w.print("[");
            boolean first = true;
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                long s = f.getEntryPoint().getOffset() - base;
                long e = f.getBody().getMaxAddress().getOffset() - base;
                String name = f.getName().replace("\\", "\\\\").replace("\"", "\\\"");
                if (!first) w.print(",");
                first = false;
                w.print("{\"name\":\"" + name + "\",\"start\":" + s + ",\"end\":" + e + "}");
            }
            w.print("]");
        }
    }
}
```

- [ ] **Step 4: Run tests**

Run: `python -m pytest debugloop/tests/test_analyze.py -v` → PASS.

- [ ] **Step 5: Start the one-time Ghidra export in the background**

Run in a visible window (it takes a long time; nothing waits on it):
```
Start-Process pwsh -ArgumentList '-NoExit','-Command','cd C:\Users\djsin\Documents\GitHub\Battleborn-Server\reborn; python -c "from debugloop import analyze; print(analyze.export_ghidra_functions())"'
```
Result is checked in Task 15 (spike S4). Triage works without it.

- [ ] **Step 6: Commit**

```bash
git add debugloop/analyze.py debugloop/ghidra/ExportFunctions.java debugloop/tests/test_analyze.py
git commit -m "feat(debugloop): triage report with cdb, Ghidra names and hook table"
```

---

### Task 12: Loop controller

**Files:**
- Create: `debugloop/loop.py`, `debugloop/tests/test_loop.py`

**Interfaces:**
- Consumes: `run.run_scenario/HarnessError/RunResult`, `build.build_mod/BuildResult`, `deploy.deploy`, `analyze.write_triage`, `scenario.ladder/find_scenario/ScenarioError`, `signature.slug`, `Ledger`, `LoopState`.
- Produces:
  - `loop.Deps(run, build, deploy, triage, git, state_dir, runs_dir)` — every outside effect is injectable for tests.
  - `loop.cmd_next(d) -> int`, `cmd_verify(d) -> int`, `cmd_giveup(d) -> int`, `cmd_status(d) -> int`, `cmd_reset_stop(d) -> int`, `cmd_build(d) -> int`
  - CLI: `python -m debugloop.loop {next|verify|giveup|status|reset-stop|build}`
  - Exit codes: `0` ok, `2` harness error, `3` stopped (needs a human), `4` ladder done, `5` gave up on a bug, `10` fix needed, `11` fix attempt failed.
  - Files: `state/brief.md` (what the fix agent reads), `state/attempt_note.md` (what the fix agent writes), `state/attempts/<slug>/<n>.diff`.

Rules:
- `next`: if stopped → 3. If a bug is open → 10. Otherwise run the current step's next scenario. Pass → count it; enough passes in a row → next step. Fail → record bug, write triage + brief → 10. A bug that was given up on blocks the ladder → stop (3).
- `verify`: refuse changes outside `reborn/` and `gamecontroller/`, or inside `reborn/BB/` → attempt failed. Build → deploy → run the bug's smoke scenario, then the full one. Pass → commit, mark fixed. Same signature → attempt failed. A *different* failure that happened later (later phase, or same phase and more seconds in) → progress: commit, mark fixed, the new failure becomes the current bug. A different, earlier failure → attempt failed (the fix broke something).
- Attempt failed: save the diff, then `git stash push -u` the changes (recoverable, never deleted). 5 failed attempts → `gave_up`.
- Harness error: not counted against the bug. 3 in a row → stop.
- Keep only the 3 newest dumps per bug.

- [ ] **Step 1: Write the failing tests `debugloop/tests/test_loop.py`**

```python
from pathlib import Path

import pytest

from debugloop import loop, run
from debugloop.build import BuildResult
from debugloop.ledger import Ledger
from debugloop.outcome import Outcome
from debugloop.run import RunResult
from debugloop.state import LoopState


class FakeGit:
    def __init__(self, status=" M reborn/Hooks.cpp\n"):
        self.calls, self.status = [], status

    def __call__(self, args):
        self.calls.append(args)
        if args[0] == "status":
            return self.status
        if args[0] == "diff":
            return "the diff"
        return ""

    def did(self, verb):
        return any(c[0] == verb for c in self.calls)


def deps(tmp_path, outcomes, build_ok=True, status=" M reborn/Hooks.cpp\n"):
    it = iter(outcomes)

    def fake_run(scn):
        o = next(it)
        if isinstance(o, Exception):
            raise o
        kind, sig, phase, elapsed = o
        rd = tmp_path / "runs" / f"r{elapsed}"
        rd.mkdir(parents=True, exist_ok=True)
        return RunResult(f"r{elapsed}", scn.name, Outcome(kind, phase=phase), sig, rd, elapsed)

    return loop.Deps(run=fake_run, build=lambda: BuildResult(build_ok, "compiler said no", None),
                     deploy=lambda: None, triage=lambda r: r.run_dir / "triage.md",
                     git=FakeGit(status), state_dir=tmp_path / "state", runs_dir=tmp_path / "runs")


PASS = ("pass", None, "playing", 300)
BUG = ("exit", "exit:3", "playing", 100)


def test_three_passes_climb_to_step_1(tmp_path):
    d = deps(tmp_path, [PASS] * 3)
    assert [loop.cmd_next(d) for _ in range(3)] == [0, 0, 0]
    assert LoopState.load(d.state_dir).step == 1


def test_failure_opens_bug_and_writes_brief(tmp_path):
    d = deps(tmp_path, [BUG])
    assert loop.cmd_next(d) == 10
    st = LoopState.load(d.state_dir)
    assert st.current_bug == "exit:3" and st.bug_scenario == "s0-solo-dojo"
    assert "exit:3" in (d.state_dir / "brief.md").read_text()
    assert Ledger.load(d.state_dir).get("exit:3").status == "fixing"
    assert loop.cmd_next(d) == 10, "next refuses to run while a bug is open"


def test_verify_pass_commits_and_closes(tmp_path):
    d = deps(tmp_path, [BUG, PASS, PASS])
    loop.cmd_next(d)
    assert loop.cmd_verify(d) == 0
    assert d.git.did("commit")
    assert Ledger.load(d.state_dir).get("exit:3").status == "fixed"
    assert LoopState.load(d.state_dir).current_bug is None


def test_same_bug_five_times_gives_up_and_stashes(tmp_path):
    d = deps(tmp_path, [BUG] * 6)
    loop.cmd_next(d)
    codes = [loop.cmd_verify(d) for _ in range(5)]
    assert codes == [11, 11, 11, 11, 5]
    assert d.git.did("stash")
    assert (d.state_dir / "attempts" / "exit_3" / "1.diff").read_text() == "the diff"
    assert Ledger.load(d.state_dir).get("exit:3").status == "gave_up"


def test_later_different_failure_is_progress(tmp_path):
    d = deps(tmp_path, [BUG, ("crash", "crash:0xc0000005:battleborn+0x10", "playing", 200)])
    loop.cmd_next(d)
    assert loop.cmd_verify(d) == 10
    L = Ledger.load(d.state_dir)
    assert L.get("exit:3").status == "fixed"
    assert LoopState.load(d.state_dir).current_bug == "crash:0xc0000005:battleborn+0x10"


def test_earlier_different_failure_is_regression(tmp_path):
    d = deps(tmp_path, [BUG, ("timeout", "timeout:startup", "startup", 240)])
    loop.cmd_next(d)
    assert loop.cmd_verify(d) == 11
    assert LoopState.load(d.state_dir).current_bug == "exit:3"


def test_forbidden_path_rejected_before_build(tmp_path):
    d = deps(tmp_path, [BUG], status=" M reborn/BB/SDK_HEADERS/Engine_classes.hpp\n")
    loop.cmd_next(d)
    assert loop.cmd_verify(d) == 11
    assert "forbidden" in Ledger.load(d.state_dir).get("exit:3").notes[-1]


def test_build_failure_is_failed_attempt(tmp_path):
    d = deps(tmp_path, [BUG], build_ok=False)
    loop.cmd_next(d)
    assert loop.cmd_verify(d) == 11
    assert "compiler said no" in Ledger.load(d.state_dir).get("exit:3").notes[-1]


def test_three_harness_errors_stop(tmp_path):
    d = deps(tmp_path, [run.HarnessError("boom")] * 3)
    assert [loop.cmd_next(d) for _ in range(3)] == [2, 2, 3]
    assert "boom" in LoopState.load(d.state_dir).stopped_reason
    assert loop.cmd_next(d) == 3
    assert loop.cmd_reset_stop(d) == 0
    assert LoopState.load(d.state_dir).stopped_reason is None


def test_given_up_bug_blocks_ladder(tmp_path):
    d = deps(tmp_path, [BUG] * 7)
    loop.cmd_next(d)
    for _ in range(5):
        loop.cmd_verify(d)
    assert loop.cmd_next(d) == 3
```

Run: `python -m pytest debugloop/tests/test_loop.py -v` → FAIL (import error).

- [ ] **Step 2: Write `debugloop/loop.py`**

```python
import argparse
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

from . import analyze, build, config, deploy, run, scenario, signature
from .ledger import Ledger
from .state import LoopState, atomic_write

MAX_ATTEMPTS = 5
MAX_HARNESS_ERRORS = 3
ALLOWED = ("reborn/", "gamecontroller/")
FORBIDDEN = ("reborn/BB/",)
PHASE_ORDER = {"startup": 0, "playing": 1}
OK, HARNESS, STOPPED, LADDER_DONE, GAVE_UP, FIX_NEEDED, ATTEMPT_FAILED = 0, 2, 3, 4, 5, 10, 11


def _git(args: list[str]) -> str:
    return subprocess.run(["git", *args], cwd=config.REPO, capture_output=True, text=True,
                          check=True).stdout


@dataclass
class Deps:
    run: Callable = field(default=run.run_scenario)
    build: Callable = field(default=build.build_mod)
    deploy: Callable = field(default=deploy.deploy)
    triage: Callable = field(default=analyze.write_triage)
    git: Callable = field(default=_git)
    state_dir: Path = config.STATE_DIR
    runs_dir: Path = config.RUNS_DIR


def _load(d):
    return LoopState.load(d.state_dir), Ledger.load(d.state_dir)


def _changed_paths(d) -> list[str]:
    return [line[3:].strip().strip('"') for line in d.git(["status", "--porcelain"]).splitlines()
            if line.strip()]


def _forbidden(d) -> list[str]:
    return [p for p in _changed_paths(d)
            if not p.startswith(("debugloop/",)) and (not p.startswith(ALLOWED) or p.startswith(FORBIDDEN))]


def _has_changes(d) -> bool:
    return any(p.startswith(ALLOWED) for p in _changed_paths(d))


def _note(d) -> str:
    p = d.state_dir / "attempt_note.md"
    return p.read_text(encoding="utf-8").strip() if p.exists() else "(no note)"


def _clear_note(d) -> None:
    (d.state_dir / "attempt_note.md").unlink(missing_ok=True)


def _harness_error(st, d, msg: str) -> int:
    st.harness_errors_in_row += 1
    if st.harness_errors_in_row >= MAX_HARNESS_ERRORS:
        st.stopped_reason = f"{MAX_HARNESS_ERRORS} harness errors in a row; last: {msg}"
        st.save(d.state_dir)
        print(f"STOPPED: {st.stopped_reason}")
        return STOPPED
    st.save(d.state_dir)
    print(f"HARNESS ERROR ({st.harness_errors_in_row}/{MAX_HARNESS_ERRORS}): {msg}")
    return HARNESS


def _prune_dumps(d, bug) -> None:
    for run_id in bug.runs[:-3]:
        for dmp in (d.runs_dir / run_id).glob("*.dmp"):
            dmp.unlink()


def _write_brief(d, st, bug, r, triage: Path) -> None:
    atomic_write(d.state_dir / "brief.md", "\n".join([
        "# Fix brief", "",
        f"- Bug: `{bug.signature}` (seen {bug.count} times in {', '.join(bug.scenarios)})",
        f"- Attempt: {st.attempts_on_current + 1} of {MAX_ATTEMPTS}",
        f"- Run folder: `{r.run_dir}`",
        f"- Triage (read first): `{triage}`",
        f"- Earlier attempts: diffs in `{d.state_dir / 'attempts' / signature.slug(bug.signature)}`",
        "", "## History", "", *[f"- {n}" for n in bug.notes], "",
        "## Rules", "",
        "- Edit only `reborn/` (never `reborn/BB/`) and `gamecontroller/`.",
        "- Do not commit, do not start the game, do not touch the game folder.",
        "- Build with `python -m debugloop.loop build` until it prints BUILD OK.",
        f"- Write 2-5 lines in `{d.state_dir / 'attempt_note.md'}`: what you changed and why.",
        "- Then stop. The loop verifies your fix by running the game.", ""]))


def _new_failure(st, L, d, scn_name: str, r) -> int:
    bug = L.record(r.signature, scn_name, r.run_id)
    _prune_dumps(d, bug)
    st.consecutive_passes = 0
    if bug.status == "gave_up":
        L.save()
        st.stopped_reason = f"blocked by a bug the AI gave up on: {bug.signature}"
        st.save(d.state_dir)
        print(f"STOPPED: {st.stopped_reason}")
        return STOPPED
    L.set_status(bug.signature, "fixing")
    L.save()
    st.current_bug, st.bug_scenario, st.attempts_on_current = bug.signature, scn_name, 0
    st.bug_phase, st.bug_elapsed_s = r.outcome.phase, r.elapsed_s
    _write_brief(d, st, bug, r, d.triage(r))
    st.save(d.state_dir)
    print(f"FIX NEEDED: {bug.signature}  brief: {d.state_dir / 'brief.md'}")
    return FIX_NEEDED


def _close_bug(st) -> None:
    st.current_bug = st.bug_scenario = st.bug_phase = None
    st.attempts_on_current, st.bug_elapsed_s = 0, 0.0


def _fixed(st, L, d, note: str) -> None:
    sig = st.current_bug
    if _has_changes(d):
        d.git(["add", "-A", "--", *ALLOWED])
        d.git(["commit", "-m", f"fix: {sig} [bb-autofix]\n\n{note}\n\n"
                               "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"])
    else:
        note += " (passed with no code change - may be flaky)"
    L.set_status(sig, "fixed", note)
    L.save()
    _close_bug(st)
    st.save(d.state_dir)
    _clear_note(d)
    print(f"FIXED {sig}")


def _attempt_failed(st, L, d, why: str) -> int:
    sig = st.current_bug
    st.attempts_on_current += 1
    L.get(sig).attempts += 1
    L.set_status(sig, "fixing", f"attempt {st.attempts_on_current} failed: {why}")
    folder = d.state_dir / "attempts" / signature.slug(sig)
    folder.mkdir(parents=True, exist_ok=True)
    (folder / f"{st.attempts_on_current}.diff").write_text(d.git(["diff", "HEAD", "--", *ALLOWED]))
    if _has_changes(d) or _forbidden(d):
        d.git(["stash", "push", "-u", "-m", f"bb-autofix failed attempt {st.attempts_on_current} {sig}"])
    _clear_note(d)
    if st.attempts_on_current >= MAX_ATTEMPTS:
        L.set_status(sig, "gave_up", f"gave up after {MAX_ATTEMPTS} attempts")
        L.save()
        _close_bug(st)
        st.save(d.state_dir)
        print(f"GAVE UP {sig}")
        return GAVE_UP
    L.save()
    st.save(d.state_dir)
    with open(d.state_dir / "brief.md", "a", encoding="utf-8") as f:
        f.write(f"\n## Attempt {st.attempts_on_current} failed\n\n{why}\n\n"
                f"Now on attempt {st.attempts_on_current + 1} of {MAX_ATTEMPTS}. Try a different idea.\n")
    print(f"ATTEMPT FAILED ({st.attempts_on_current}/{MAX_ATTEMPTS}): {why[:300]}")
    return ATTEMPT_FAILED


def cmd_next(d: Deps) -> int:
    st, L = _load(d)
    if st.stopped_reason:
        print(f"STOPPED: {st.stopped_reason}")
        return STOPPED
    if st.current_bug:
        print(f"FIX NEEDED: {st.current_bug}  brief: {d.state_dir / 'brief.md'}")
        return FIX_NEEDED
    scns = scenario.ladder(st.step)
    scn = scns[st.scenario_index % len(scns)]
    try:
        r = d.run(scn)
    except run.HarnessError as e:
        return _harness_error(st, d, str(e))
    st.harness_errors_in_row = 0
    if r.outcome.kind != "pass":
        return _new_failure(st, L, d, scn.name, r)
    st.consecutive_passes += 1
    st.scenario_index += 1
    msg = f"PASS {scn.name} ({st.consecutive_passes}/{scn.required_passes} in a row at step {st.step})"
    if st.consecutive_passes >= scn.required_passes:
        st.step, st.consecutive_passes, st.scenario_index = st.step + 1, 0, 0
        msg += f" -> NOW AT STEP {st.step}"
    if st.step > 3:
        st.stopped_reason = "steps 0-3 complete; step 4 needs human players"
    st.save(d.state_dir)
    print(msg)
    return LADDER_DONE if st.step > 3 else OK


def _smoke_of(scn):
    if scn.smoke:
        return None
    try:
        return scenario.find_scenario(scn.name + "-smoke")
    except scenario.ScenarioError:
        return None


def cmd_verify(d: Deps) -> int:
    st, L = _load(d)
    if not st.current_bug:
        print("nothing to verify")
        return OK
    note = _note(d)
    bad = _forbidden(d)
    if bad:
        return _attempt_failed(st, L, d, f"touched forbidden paths {bad}. {note}")
    b = d.build()
    if not b.ok:
        return _attempt_failed(st, L, d, f"build failed: {b.output[-1500:]}")
    d.deploy()
    scn = scenario.find_scenario(st.bug_scenario)
    r = None
    for s in [x for x in (_smoke_of(scn), scn) if x]:
        try:
            r = d.run(s)
        except run.HarnessError as e:
            return _harness_error(st, d, str(e))
        if r.outcome.kind != "pass":
            break
    st.harness_errors_in_row = 0
    if r.outcome.kind == "pass":
        _fixed(st, L, d, note)
        return OK
    if r.signature == st.current_bug:
        return _attempt_failed(st, L, d, f"same bug again in run {r.run_id}. Note was: {note}")
    later = (PHASE_ORDER.get(r.outcome.phase, 0), r.elapsed_s) > \
            (PHASE_ORDER.get(st.bug_phase or "startup", 0), st.bug_elapsed_s)
    if later:
        _fixed(st, L, d, f"{note} (got further; next failure {r.signature})")
        return _new_failure(st, L, d, r.scenario, r)
    return _attempt_failed(st, L, d, f"new, earlier failure {r.signature} in run {r.run_id}. Note was: {note}")


def cmd_giveup(d: Deps) -> int:
    st, L = _load(d)
    if not st.current_bug:
        return OK
    st.attempts_on_current = MAX_ATTEMPTS - 1
    return _attempt_failed(st, L, d, "fix agent gave up")


def cmd_status(d: Deps) -> int:
    st, _ = _load(d)
    print(st)
    md = d.state_dir / "ledger.md"
    print(md.read_text(encoding="utf-8") if md.exists() else "(no bugs yet)")
    return OK


def cmd_reset_stop(d: Deps) -> int:
    st, _ = _load(d)
    st.stopped_reason, st.harness_errors_in_row = None, 0
    st.save(d.state_dir)
    return OK


def cmd_build(d: Deps) -> int:
    r = d.build()
    print("BUILD OK" if r.ok else "BUILD FAILED\n" + r.output[-6000:])
    return OK if r.ok else 1


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="python -m debugloop.loop")
    ap.add_argument("command", choices=["next", "verify", "giveup", "status", "reset-stop", "build"])
    a = ap.parse_args(argv)
    fn = {"next": cmd_next, "verify": cmd_verify, "giveup": cmd_giveup, "status": cmd_status,
          "reset-stop": cmd_reset_stop, "build": cmd_build}[a.command]
    return fn(Deps())


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 3: Run tests**

Run: `python -m pytest debugloop/tests -v` → all PASS.

- [ ] **Step 4: Commit**

```bash
git add debugloop/loop.py debugloop/tests/test_loop.py
git commit -m "feat(debugloop): loop controller (next/verify/giveup/status)"
```

---

### Task 13: The `bb-autofix` skill

**Files:**
- Create: `.claude/skills/bb-autofix/SKILL.md`

**Interfaces:**
- Consumes: `python -m debugloop.loop ...` exit codes from Task 12.
- Produces: one loop iteration per invocation; meant to run as `/loop /bb-autofix`.

- [ ] **Step 1: Write `.claude/skills/bb-autofix/SKILL.md`**

````markdown
---
name: bb-autofix
description: One iteration of the Battleborn autonomous debug loop - run the game test, and if it finds a bug, have a Fable subagent fix it and verify. Run as `/loop /bb-autofix`.
---

# bb-autofix: one loop iteration

Work from the repo root `C:\Users\djsin\Documents\GitHub\Battleborn-Server\reborn`, on branch `agent/autofix`. Never push. Never change Windows settings. Never close a game window the loop did not start.

## 1. Run the next test

Run `python -m debugloop.loop next` (timeout: 45 minutes; run it in the background and wait for the notification if your tool limit is shorter). Act on the exit code:

| Exit | Meaning | Do |
|---|---|---|
| 0 | test passed | If the output says `NOW AT STEP`, send a push notification: "Battleborn loop reached step N". End the iteration. |
| 2 | harness error | Read the message. If it says a game is already running, push-notify the user once ("close the game so the loop can continue") and end the iteration. Otherwise end the iteration (the loop retries). |
| 3 | stopped | Read `python -m debugloop.loop status`. If the reason is harness errors with a Python traceback, go to section 4. Otherwise push-notify the reason and stop the loop. |
| 4 | ladder done | Push-notify "Steps 0-3 pass. Ready for human players." Stop the loop. |
| 10 | bug found | Go to section 2. |

## 2. Fix (Fable subagent)

Dispatch one subagent with the Agent tool, `model: "fable"`, `subagent_type: "general-purpose"`, foreground. Prompt:

> You are fixing one bug in the Battleborn Reborn mod (C++ DLL injected into a 2016 Unreal Engine 3 game) or its C# lobby server. Repo: `C:\Users\djsin\Documents\GitHub\Battleborn-Server\reborn`. Read `debugloop/state/brief.md` first, then the triage file it names, then the run folder logs. Use the superpowers:systematic-debugging skill: form a hypothesis from the evidence before editing. Follow every rule in the brief. Make the smallest change that plausibly fixes the root cause; prefer guarding the exact failing path over broad rewrites. Build with `python -m debugloop.loop build` until BUILD OK. Write your note to `debugloop/state/attempt_note.md`. If you are certain the bug cannot be fixed from mod code, write why in the note and run `python -m debugloop.loop giveup`. Reply with 3 lines: hypothesis, change, confidence.

## 3. Verify

Run `python -m debugloop.loop verify` (timeout 45 minutes, background if needed).

| Exit | Do |
|---|---|
| 0 | Fixed and committed. End the iteration. |
| 10 | Progress: fixed one bug, a new later one is open. Go back to section 2 in this same iteration (at most 3 fix cycles per iteration). |
| 11 | Attempt failed (brief now has the reason). Go back to section 2 (at most 3 fix cycles per iteration). |
| 5 | Gave up on this bug. Push-notify "Gave up on bug <signature> after 5 tries". End the iteration. |
| 2 / 3 | As in section 1. |

## 4. Harness repair (Sonnet subagent)

Only when the loop stopped on harness errors with a Python traceback in `debugloop/`. Dispatch a subagent, `model: "sonnet"`: "The test harness in `debugloop/` failed: <message>. Reproduce with pytest, fix `debugloop/` only, keep all tests passing (`python -m pytest debugloop/tests -v`), commit with message `fix(debugloop): ...`." Then run `python -m debugloop.loop reset-stop` and end the iteration. If the same traceback happens again after a repair, push-notify the user and stop the loop.

## Every iteration ends with

One line to the user: step, passes in a row, open bug (if any), what happened.
````

- [ ] **Step 2: Commit**

```bash
git add .claude/skills/bb-autofix/SKILL.md
git commit -m "feat: bb-autofix skill for the unattended loop"
```

---

### Task 14: First live runs (solo) and harness self-test on the real game

Exploratory: these steps run the real game. Expected results are given; when reality differs, fix the cause (in the mod or harness, with tests where possible) and re-run the step.

**Files:** whatever the fixes touch. Record surprises in the spec's "Risks" section.

- [ ] **Step 1: Close any running Battleborn. Build and deploy.**

Run: `python -m debugloop.loop build` → `BUILD OK`. Then `python -c "from debugloop import deploy; print(deploy.deploy())"`.

- [ ] **Step 2: Solo smoke run**

Run: `python -m debugloop.run s0-solo-dojo-smoke`
Watch the game window. Expected: main menu appears, autopilot logs `[AUTOPILOT] phase menu_ready` → `launching` → `playing` in `debugloop/runs/<id>/solo.log`, character spawns in the Dojo, run ends `pass` after 120 s.
If the profile/save screen blocks the menu: check `EnsureSaveLoaded` runs (log line) and fix.
If the pawn stands still: note it for spike S1 (Task 15). A standing pawn still passes step 0.

- [ ] **Step 3: Crash self-test**

Run: `python -m debugloop.run selftest-crash`
Expected: exit 1, `result.json` outcome `crash`, signature starting `crash:0xc0000005:reborn+0x`, a `.dmp` over 1 MB. Then `python -m debugloop.analyze <run dir>` → `triage.md` whose cdb section names `Autopilot::Tick` (proves reborn.pdb symbols resolve).

- [ ] **Step 4: Freeze self-test**

Run: `python -m debugloop.run selftest-hang`
Expected: within ~40 s of play, outcome `hang`, `solo.hang.json` present, frames include a `reborn+` frame (the `Sleep` call in `Autopilot::Tick`).

- [ ] **Step 5: Commit fixes**

```bash
git add -A reborn debugloop
git commit -m "fix: issues found in first live runs"
```
(Skip if nothing changed.)

---

### Task 15: Live bring-up of multiplayer (spikes S1–S4) and a hand-run of the loop

- [ ] **Step 1 (S2): Two identities.** Run `python -m debugloop.run s1-dojo-1client-smoke`.
Expected: server window starts, `server.log` shows listening on 7777, client joins, character select is locked in by the autopilot, `/state` of the server lists `c1` in `player_locations`.
If the client shows the same Steam name as another instance or the loader errors: read the gbe_fork README in `Win64` for where the steamclient reads `steam_settings` in ColdClientLoader mode, adjust `ensure_identity`, add a test in `test_run.py` for the new file layout, re-run.

- [ ] **Step 2 (S3): Memory for three instances.** Run `python -m debugloop.run s2-algorithm-2clients-smoke`. During the run, read `memory_mb` from the timeline. Expected: the three together stay below total RAM minus 2 GB (check with `(Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory`).
If not: add `-nullrhi` to the server's args in `launch.GAME_BASE_ARGS` handling (server only) and lower clients to `-ResX=640 -ResY=360`; re-run.

- [ ] **Step 3 (S1): Steering.** In the step 2 run's timeline, `pawn_location` of `c1` and `c2` must change by more than 500 units over the run.
If not: move the axis writes from `BeforePlayerTick` to the end of `Autopilot::Tick`; if still not, set `pc->Pawn->Acceleration` toward the pawn's facing direction scaled by `AccelRate` each tick in `BeforePlayerTick`. Survival scenarios still pass without movement, so this never blocks the ladder; note the result in the spec.

- [ ] **Step 4 (S4): Ghidra.** When the background export from Task 11 finishes, check `debugloop/ghidra/out/battleborn_functions.json`: expected more than 20 000 functions, and the RVAs of `GameEngineTickHook` (0x207e10) and `ProcessEventHook` (0x109ca0) each fall at a function start.
If Arxan protection made the analysis useless (few functions, or the hook RVAs land mid-function): leave the file absent (triage works without it) and record this in the spec.

- [ ] **Step 5: Hand-run the loop.** Run `python -m debugloop.loop next` repeatedly (by hand, one at a time) until either step 2 is reached or a bug opens. When a bug opens: follow `state/brief.md` yourself exactly as the Fable agent would (read triage, fix, `loop build`, write the note), then `python -m debugloop.loop verify`. This proves the full cycle before it runs unattended.

- [ ] **Step 6: Commit**

```bash
git add -A reborn debugloop docs
git commit -m "chore: live bring-up results (spikes S1-S4)"
```

---

### Task 16: Start the unattended loop

- [ ] **Step 1:** Tell the user in plain words that the loop is starting, what they will be notified about (each new step reached, a bug given up on, the loop stopping), and that they can stop it any time by typing in this chat.
- [ ] **Step 2:** Keep the laptop awake: request keep-awake through the app (`mcp__ccd_host__request_keep_awake`). Do not change Windows power settings.
- [ ] **Step 3:** Start `/loop /bb-autofix` (dynamic pacing; each iteration is one test plus any fix).
- [ ] **Step 4:** After the first full iteration, send the user a one-paragraph status: step, passes, bugs found and fixed.

---

## Self-Review Notes

- Spec coverage: launch flags (T5), crash/freeze capture incl. first-chance VEH and 60 s watchdog (T6), localhost debug server (T7), autopilot (T8), runner with own-process-only kill, `active.json`, foreign-game refusal, disk check (T10), grading incl. desync 1500/10 s and startup 240 s (T3), signatures (T3), ledger + retention (T4, T10, T12), triage with cdb/Ghidra/hook table (T11), loop with 5 attempts, 3 harness errors, progress rule, stash-not-delete revert, allowed paths (T12), skill with Fable fixer and Sonnet harness repair (T13), spikes S1–S4 (T15), ladder steps 0–3 (T2 scenarios, T12), step 4 left to humans.
- Known soft spots, settled live: SDK field names in `GameState.cpp`/`Autopilot.cpp` (grep instructions given), Steam identity layout (S2), steering (S1).

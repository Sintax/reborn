# Combat Autopilot and Play MCP Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the test players an in-game combat brain (find, aim, shoot, skills, chase/retreat) and give Claude tools (MCP + CLI) to look at a test game, read its situation, and give it orders.

**Architecture:** A new `Combat` module in the mod DLL runs every tick inside `Autopilot::PlayTick`, keeps a cached enemy census, and supplies the movement/aim axes that `Autopilot::WriteAxes` already writes. Three new routes on the existing per-instance debug HTTP server (`/combat`, `/order`, `/act`) expose it. On the Python side `debugloop/play.py` wraps those routes (library + CLI), `debugloop/mcp_server.py` exposes them as MCP tools, and `debugloop/playsession.py` adds `loop play` / `loop stop-play` so a scenario can be left running for live driving.

**Tech Stack:** C++17 (MSVC, UE3 SDK headers, cpp-httplib, nlohmann json), Python 3.14 (urllib, Pillow, `mcp` FastMCP), pytest.

**Spec:** `docs/superpowers/specs/2026-10-06-combat-autopilot-and-play-mcp-design.md`

## Global Constraints

- Branch `agent/autofix`; never push; never touch `reborn/BB/` (generated SDK).
- Build the DLL with `python -m debugloop.loop build` (run in the background, 110-minute timeout) until it prints `BUILD OK`. New `.cpp` files must be added to `reborn/reborn.vcxproj` (`<ClCompile Include="X.cpp" />` next to `Autopilot.cpp` at line 202) or they do not compile.
- Python tests: `python -m pytest debugloop/tests -q` from the repo root `C:\Users\djsin\Documents\GitHub\Battleborn-Server\reborn`. All must pass before each commit.
- Game objects may only be touched on the game thread: HTTP handlers go through `DebugServer::RunOnGameThread` (already the case for every `Answer(...)` call).
- The combat brain is client-only: `Autopilot::Active()` is already false on the server; never add a server path.
- Nothing in Python may start or stop a game except `run.py` (runs) and `playsession.py` (play sessions). The MCP server never launches processes.
- All HTTP is `127.0.0.1` only. Debug ports start at `config.FIRST_DEBUG_PORT` (18080).
- The `-rbcombat` launch flag defaults to off, so scenarios that do not name it behave exactly as today.
- Commit messages: imperative, prefixed `feat:`/`fix:`/`test:`/`docs:`, ending with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.
- Never launch the game by hand during a task unless the task says "live check"; the loop's `active.json` / `play.json` must be the only owners of game processes.

## Review Focus

1. A pawn in the census is destroyed between the census and the aim tick (a bot dies) — the brain must drop the target, not read freed memory. Pinned in Task 2 by the `Gone()` re-check on every tick (code shown) and in the live check by watching `[COMBAT]` target-drop lines.
2. `/order` with a body that is not JSON, or a mode string the brain does not know — must answer 400 with `{"error":...}` and leave the previous order untouched. Pinned in Task 4 (C++ shown) and Task 6 (`test_order_rejects_unknown_mode` against the fake game, which mirrors the contract).
3. `/act` while the player is dead (no pawn) — must answer 409 `{"error":"no_pawn"}` rather than crash on a null pawn. Pinned in Task 4 and Task 6 (`test_act_without_pawn_is_a_plain_error`).
4. `loop next` while a play session is alive — must refuse without counting a harness error or killing the session. Pinned in Task 8 (`test_next_refuses_while_play_session_alive`).
5. The `look` tool when the game window is minimised or gone — must return "no picture" text, never a traceback into the MCP transport. Pinned in Task 6 (`test_look_without_window_returns_none`) and Task 7 (`test_bb_look_without_window_is_text`).

---

### Task 1: `Combat` census and the read-only `/combat` route

**Files:**
- Create: `reborn/Combat.hpp`, `reborn/Combat.cpp`
- Create: `reborn/PawnUtils.hpp` (shared pawn helpers moved out of `GameState.cpp`)
- Modify: `reborn/GameState.cpp:51-66` (use `PawnUtils`), `reborn/LaunchOptions.hpp:10-20`, `reborn/LaunchOptions.cpp:49`, `reborn/DebugServer.hpp`, `reborn/DebugServer.cpp:54-77`, `reborn/dllmain.cpp:122`, `reborn/Autopilot.cpp:181-213` (PlayTick), `reborn/reborn.vcxproj:202`
- Test: live check on the solo training map (no C++ unit harness exists)

**Interfaces:**
- Consumes: `SDKUtils::GetLocalPlayerController()`, `Globals::GetGWorld()`, `Engine::ExecConsoleCommand`, `DebugServer::RunOnGameThread` (internal), `LaunchOptions::Get()`.
- Produces (used by Tasks 2-4):
  ```cpp
  namespace Combat {
      struct Enemy { uintptr_t id; std::string hero; std::string kind; float loc[3]; float distance; float bearingDeg; float pitchDeg; bool visible; float health; int team; };
      bool Enabled();
      void SetEnabled(bool on);
      void Tick(float dt, APoplarPlayerController* pc);   // every play tick with a live pawn
      void OnNoPawn();                                      // every play tick without a pawn
      std::string SnapshotJson();                           // GET /combat
      nlohmann::json StatsJson();                           // for /state "combat"
  }
  ```
  and `DebugServer::Routes { StateFn state; ExecFn exec; BodyFn combat, order, act; }` with `using BodyFn = std::function<std::string(const std::string& body)>;` where a BodyFn returning a string that starts with `{"error"` is sent with the status embedded as described in Task 4.

- [ ] **Step 1: Add the launch flag**

In `reborn/LaunchOptions.hpp` after `bool autopilot = false;` add:
```cpp
        bool combat = false;          // client: autopilot fights (Combat.cpp) instead of wandering
```
In `reborn/LaunchOptions.cpp` after the `-rbautopilot` line add:
```cpp
            else if (key == L"-rbcombat") o.combat = true;
```

- [ ] **Step 2: Move the shared pawn helpers into `PawnUtils.hpp`**

Create `reborn/PawnUtils.hpp`:
```cpp
#pragma once
#include "BB/SdkHeaders.hpp"
#include <string>

// Pawn checks shared by GameState (the /state snapshot) and Combat (the enemy census).
namespace PawnUtils {
    constexpr uint64_t kPendingKill = 0x2000000000000000ull;

    inline bool Gone(UObject* o) { return !o || (o->ObjectFlags & kPendingKill); }

    inline bool IsDefault(UObject* o) { return o->GetFullName().find("Default__") != std::string::npos; }

    // A live pawn in a level (not an archetype or class default, not being destroyed).
    inline bool LivePawnInWorld(APawn* p) {
        return !Gone(p) && !p->bDeleteMe && !p->bTearOff && p->Outer && p->Outer->IsA(ULevel::StaticClass());
    }

    inline std::string HeroOf(APawn* p) {
        if (p->IsA(APoplarPawn::StaticClass())) {
            UPlayerClassDefinition* def = reinterpret_cast<APoplarPawn*>(p)->PoplarPlayerClassDef;
            if (!Gone(def)) return def->GetName();
        }
        return !Gone(p->ObjectArchetype) ? p->ObjectArchetype->GetName() : std::string("unknown");
    }

    // "player" = a person, "bot" = a hero played by the AI, "minion" = everything else.
    inline std::string KindOf(APawn* p) {
        APlayerReplicationInfo* pri = p->PlayerReplicationInfo;
        if (Gone(pri)) return "minion";
        return pri->bBot ? "bot" : "player";
    }
}
```
Note: `GameState.cpp` currently names the class-definition type `UPoplarPlayerClassDefinition`; keep whichever name `GameState.cpp:81` uses (copy it verbatim) — it is the type of `APoplarPawn::PoplarPlayerClassDef` in the SDK.

In `reborn/GameState.cpp`: add `#include "PawnUtils.hpp"`, delete its own `kPendingKill`, `Gone`, `LivePawnInWorld`, `HeroOf`, `IsDefault` definitions (lines 28 and 56-86), and add `using PawnUtils::Gone; using PawnUtils::IsDefault; using PawnUtils::LivePawnInWorld; using PawnUtils::HeroOf;` inside its anonymous namespace so the rest of the file compiles unchanged.

- [ ] **Step 3: Write `Combat.hpp`**

```cpp
#pragma once
#include "BB/SdkHeaders.hpp"
#include "json.hpp"
#include <string>

// The autopilot's combat brain: enemy census, target choice, aim, fire, skills, movement intent.
// Client only. Runs on the game thread from Autopilot::PlayTick.
namespace Combat {
    struct Enemy {
        uintptr_t id;          // the pawn pointer, the stable id the tools use
        std::string hero;
        std::string kind;      // player | bot | minion
        float loc[3];
        float distance;
        float bearingDeg;      // -180..180 relative to the view yaw; positive = to the right
        float pitchDeg;        // positive = above the eye line
        bool visible;
        float health;
        int team;
    };

    bool Enabled();
    void SetEnabled(bool on);
    void Tick(float dt, APoplarPlayerController* pc);
    void OnNoPawn();
    std::string SnapshotJson();
    nlohmann::json StatsJson();
}
```

- [ ] **Step 4: Write `Combat.cpp` (census + snapshot only; aim/orders come in Tasks 2-4)**

```cpp
#include "Combat.hpp"
#include "Engine.hpp"
#include "LaunchOptions.hpp"
#include "PawnUtils.hpp"
#include "Utils.hpp"
#include <cmath>
#include <cstdio>
#include <vector>

namespace Combat {
    namespace {
        using PawnUtils::Gone;

        constexpr float kPi = 3.14159265f;
        constexpr float kUnitsPerDeg = 65536.f / 360.f;   // UE3 rotator units
        constexpr float kCensusEvery = 0.25f;
        constexpr int kMaxEnemiesInJson = 12;

        bool g_enabled = false;
        bool g_enabledInit = false;
        float g_sinceCensus = 1e9f;
        std::vector<Enemy> g_enemies;      // nearest first, refreshed every kCensusEvery
        APoplarPlayerController* g_pc = nullptr;
        float g_myLoc[3] = {0, 0, 0};
        int g_myYaw = 0, g_myPitch = 0;
        float g_health = 0.f, g_maxHealth = 0.f;
        std::string g_hero;

        struct Stats { int shots = 0, skills = 0, kills = 0, deaths = 0; float damageTaken = 0.f, withTargetS = 0.f, firingS = 0.f; };
        Stats g_stats;
        float g_lastHealth = -1.f;

        int WrapUnits(int d) { d &= 0xFFFF; return d > 32767 ? d - 65536 : d; }

        FVector Eye(APawn* p) { return FVector{ p->Location.X, p->Location.Y, p->Location.Z + p->BaseEyeHeight }; }
        FVector AimPoint(APawn* p) { return FVector{ p->Location.X, p->Location.Y, p->Location.Z + p->BaseEyeHeight * 0.5f }; }

        void Census(APoplarPlayerController* pc) {
            g_enemies.clear();
            APawn* me = pc->Pawn;
            if (Gone(me) || !me->IsA(APoplarPawn::StaticClass())) return;
            APoplarPawn* mePop = reinterpret_cast<APoplarPawn*>(me);
            AWorldInfo* wi = pc->WorldInfo;
            if (Gone(wi)) return;
            FVector eye = Eye(me);
            int viewYaw = pc->Rotation.Yaw;
            int guard = 0;
            for (APawn* p = wi->PawnList; p && guard < 2000; p = p->NextPawn, guard++) {
                if (p == me || !PawnUtils::LivePawnInWorld(p)) continue;
                if (p->GetHealth() <= 0.f) continue;
                if (!mePop->IsEnemy(p)) continue;
                FVector aim = AimPoint(p);
                float dx = aim.X - eye.X, dy = aim.Y - eye.Y, dz = aim.Z - eye.Z;
                float flat = std::sqrt(dx * dx + dy * dy);
                Enemy e;
                e.id = reinterpret_cast<uintptr_t>(p);
                e.hero = PawnUtils::HeroOf(p);
                e.kind = PawnUtils::KindOf(p);
                e.loc[0] = p->Location.X; e.loc[1] = p->Location.Y; e.loc[2] = p->Location.Z;
                e.distance = std::sqrt(flat * flat + dz * dz);
                int yawTo = (int)std::lround(std::atan2(dy, dx) * 180.f / kPi * kUnitsPerDeg);
                e.bearingDeg = WrapUnits(yawTo - viewYaw) / kUnitsPerDeg;
                e.pitchDeg = std::atan2(dz, flat) * 180.f / kPi;
                e.visible = me->FastTrace(aim, eye, FVector{0, 0, 0}, false);
                e.health = p->GetHealth();
                e.team = p->GetTeamNum();
                g_enemies.push_back(e);
            }
            std::sort(g_enemies.begin(), g_enemies.end(),
                      [](const Enemy& a, const Enemy& b) { return a.distance < b.distance; });
        }

        void TrackStats(float dt, APoplarPlayerController* pc) {
            float h = pc->Pawn->GetHealth();
            if (g_lastHealth >= 0.f && h < g_lastHealth) g_stats.damageTaken += g_lastHealth - h;
            g_lastHealth = h;
            APlayerReplicationInfo* pri = pc->PlayerReplicationInfo;
            if (!Gone(pri)) { g_stats.kills = pri->Kills; g_stats.deaths = pri->Deaths; }
        }
    }

    bool Enabled() {
        if (!g_enabledInit) { g_enabled = LaunchOptions::Get().combat; g_enabledInit = true; }
        return g_enabled;
    }
    void SetEnabled(bool on) { g_enabledInit = true; g_enabled = on; }

    void Tick(float dt, APoplarPlayerController* pc) {
        g_pc = pc;
        APawn* me = pc->Pawn;
        g_myLoc[0] = me->Location.X; g_myLoc[1] = me->Location.Y; g_myLoc[2] = me->Location.Z;
        g_myYaw = pc->Rotation.Yaw; g_myPitch = pc->Rotation.Pitch;
        g_health = me->GetHealth(); g_maxHealth = me->GetMaxHealth();
        g_hero = PawnUtils::HeroOf(me);
        TrackStats(dt, pc);
        g_sinceCensus += dt;
        if (g_sinceCensus >= kCensusEvery) { g_sinceCensus = 0.f; Census(pc); }
    }

    void OnNoPawn() {
        g_enemies.clear();
        g_lastHealth = -1.f;
        g_health = 0.f;
    }

    nlohmann::json StatsJson() {
        return { {"shots", g_stats.shots}, {"skills_used", g_stats.skills}, {"kills", g_stats.kills},
                 {"deaths", g_stats.deaths}, {"damage_taken", g_stats.damageTaken},
                 {"time_with_target_s", g_stats.withTargetS}, {"time_firing_s", g_stats.firingS} };
    }

    std::string SnapshotJson() {
        nlohmann::json j;
        j["enabled"] = Enabled();
        j["mode"] = "wander";   // Task 3 replaces this with the real mode
        bool alive = g_pc && !Gone(g_pc) && !Gone(g_pc->Pawn);
        j["me"] = { {"hero", g_hero}, {"location", {g_myLoc[0], g_myLoc[1], g_myLoc[2]}},
                    {"yaw", g_myYaw / kUnitsPerDeg}, {"pitch", WrapUnits(g_myPitch) / kUnitsPerDeg},
                    {"health", g_health}, {"max_health", g_maxHealth}, {"dead", !alive} };
        j["target"] = nullptr;   // Task 2
        nlohmann::json arr = nlohmann::json::array();
        int n = 0;
        for (const Enemy& e : g_enemies) {
            if (n++ >= kMaxEnemiesInJson) break;
            char id[32]; std::snprintf(id, sizeof id, "%llx", (unsigned long long)e.id);
            arr.push_back({ {"id", id}, {"hero", e.hero}, {"kind", e.kind}, {"distance", e.distance},
                            {"bearing", e.bearingDeg}, {"pitch", e.pitchDeg}, {"visible", e.visible},
                            {"health", e.health}, {"team", e.team},
                            {"location", {e.loc[0], e.loc[1], e.loc[2]}} });
        }
        j["enemies"] = arr;
        j["n_enemies"] = g_enemies.size();
        j["stats"] = StatsJson();
        return j.dump();
    }
}
```
Add `#include <algorithm>` for `std::sort`. Add `<ClCompile Include="Combat.cpp" />` after the `Autopilot.cpp` line in `reborn/reborn.vcxproj`.

- [ ] **Step 5: Add the route to the debug server**

`reborn/DebugServer.hpp` becomes:
```cpp
#pragma once
#include <functional>
#include <string>

namespace DebugServer {
    using StateFn = std::function<std::string()>;
    using ExecFn = std::function<std::string(const std::string&)>;
    using BodyFn = std::function<std::string(const std::string& body)>;   // JSON in, JSON out

    struct Routes {
        StateFn state;   // GET /state
        ExecFn exec;     // POST /exec (raw console command)
        BodyFn combat;   // GET /combat
        BodyFn order;    // POST /order
        BodyFn act;      // POST /act
    };

    bool Start(int port, const Routes& routes);
    void Pump();   // game thread, once per tick
    void Stop();
}
```
In `reborn/DebugServer.cpp`: replace `StateFn g_state; ExecFn g_exec;` with `Routes g_routes;`, change `Start` to `bool Start(int port, const Routes& routes) { g_routes = routes; ...`, use `g_routes.state` / `g_routes.exec` where `g_state` / `g_exec` were, and add after the `/exec` route:
```cpp
        g_server->Get("/combat", [](const httplib::Request&, httplib::Response& res) {
            AnswerJson(res, [] { return g_routes.combat(""); });
        });
        g_server->Post("/order", [](const httplib::Request& req, httplib::Response& res) {
            std::string body = req.body;
            AnswerJson(res, [body] { return g_routes.order(body); });
        });
        g_server->Post("/act", [](const httplib::Request& req, httplib::Response& res) {
            std::string body = req.body;
            AnswerJson(res, [body] { return g_routes.act(body); });
        });
```
and in the anonymous namespace, after `Answer`:
```cpp
        // Like Answer, but a JSON body of the form {"error":"...","status":NNN} sets the HTTP status.
        void AnswerJson(httplib::Response& res, std::function<std::string()> fn) {
            std::string out;
            try {
                if (!RunOnGameThread(std::move(fn), out)) return Unresponsive(res);
            }
            catch (const std::exception& e) {
                res.status = 500;
                res.set_content(std::string("{\"error\":\"") + e.what() + "\"}", "application/json");
                return;
            }
            if (out.rfind("{\"error\"", 0) == 0) {
                size_t at = out.find("\"status\":");
                res.status = at == std::string::npos ? 400 : std::atoi(out.c_str() + at + 9);
            }
            res.set_content(out, "application/json");
        }
```
In `reborn/dllmain.cpp:122` replace the `DebugServer::Start(...)` call with:
```cpp
        if (opt.debugPort) DebugServer::Start(opt.debugPort, DebugServer::Routes{
            GameState::SnapshotJson, GameState::Exec,
            [](const std::string&) { return Combat::SnapshotJson(); },
            [](const std::string&) { return std::string("{\"error\":\"not_implemented\",\"status\":501}"); },
            [](const std::string&) { return std::string("{\"error\":\"not_implemented\",\"status\":501}"); } });
```
and add `#include "Combat.hpp"` at the top of `dllmain.cpp`.

- [ ] **Step 6: Call the brain from the autopilot**

In `reborn/Autopilot.cpp` add `#include "Combat.hpp"`. In `PlayTick`, inside the `if (!pc->Pawn) { ... }` block add `Combat::OnNoPawn();` as its first line, and right after `if (!g_hadPawn) { g_hadPawn = true; ResetStuck(pc); }` add:
```cpp
            if (Combat::Enabled()) Combat::Tick(dt, pc);
```
(The wander logic below it stays for now; Task 3 makes it conditional.)

- [ ] **Step 7: Expose stats in `/state`**

In `GameState::SnapshotJson`, in the client branch (after `j["pawn_health"] = ...`) add:
```cpp
            j["combat"] = { {"enabled", Combat::Enabled()}, {"stats", Combat::StatsJson()} };
```
with `#include "Combat.hpp"` at the top of `GameState.cpp`.

- [ ] **Step 8: Build**

Run in the background with a 110-minute timeout: `python -m debugloop.loop build`. Expected: `BUILD OK`. Fix compile errors before going on (likely spots: the `FVector` brace-init if the SDK struct has a constructor — then use `FVector v; v.X=...;`; `IsEnemy` taking `UObject*` — pass `p` which converts implicitly).

- [ ] **Step 9: Live check (solo Dojo, combat flag on)**

Ask the user to confirm no loop run is active (`debugloop/runs/active.json` absent and `tasklist | findstr Battleborn` empty). Then from the repo root:
```bash
python -m debugloop.run s0-solo-dojo-smoke --poll 5
```
after temporarily adding `"-rbcombat"` to the `args` of `debugloop/scenarios/s0-solo-dojo-smoke.toml` (revert it before committing; s0 stays random per spec). While it runs (about 4 minutes), from another shell:
```bash
curl -s http://127.0.0.1:18080/combat
```
Expected: JSON with `"enabled": true`, `me.health > 0`, and an `enemies` array. On the Dojo the training dummies may or may not appear as enemies; write down what appears (hero names, kinds, teams) in `docs/notes/combat-census-notes.md`. If the array is always empty while bots are visibly present on screen, log `p->GetTeamNum()` for the first 5 pawns and `me->GetTeamNum()` for the next build; the fallback is `e.team != myTeam` instead of `IsEnemy`.

- [ ] **Step 10: Commit**

```bash
git add reborn/Combat.hpp reborn/Combat.cpp reborn/PawnUtils.hpp reborn/GameState.cpp reborn/LaunchOptions.hpp reborn/LaunchOptions.cpp reborn/DebugServer.hpp reborn/DebugServer.cpp reborn/dllmain.cpp reborn/Autopilot.cpp reborn/reborn.vcxproj docs/notes/combat-census-notes.md
git commit -m "feat(combat): enemy census and the /combat route"
```

---

### Task 2: Aim, fire, target selection, calibration

**Files:**
- Modify: `reborn/Combat.hpp`, `reborn/Combat.cpp`, `reborn/Autopilot.cpp:164-179` (WriteAxes)

**Interfaces:**
- Produces: `Combat::Axes { float forward, strafe, turn, lookUp; bool valid; }` and `Combat::Axes CurrentAxes();` — the autopilot writes these when `valid`.
- `Combat::Tuning` struct (defaults in code) read by Task 3/4.

- [ ] **Step 1: Add the public pieces to `Combat.hpp`**

```cpp
    struct Axes { float forward = 1.f, strafe = 0.f, turn = 0.f, lookUp = 0.f; bool valid = false; };
    Axes CurrentAxes();

    struct Tuning {
        float fireRange = 3000.f;     // fire only inside this (units; ~30 m)
        float engageRange = 1200.f;   // hunt: approach until this close
        float aimOnDeg = 4.f;         // aim counts as on under this yaw+pitch error
        float maxTurnDegPerTick = 20.f;
        float retreatHealthFrac = 0.25f;
        float retreatNearUnits = 800.f;
        float playerWeight = 0.6f;    // players score closer than bots/minions
        float targetMemoryS = 3.f;    // drop a target unseen this long
    };
    Tuning& GetTuning();
```

- [ ] **Step 2: Calibration + aim in `Combat.cpp`**

Add to the anonymous namespace:
```cpp
        Tuning g_tuning;

        // How many rotator units one unit of aTurn / aLookUp moves the view in one frame. Measured
        // on the first ticks with a pawn: write 1.0 for a frame, read the change. The sign comes
        // out of the measurement too (the engine may invert look-up).
        float g_yawPerInput = 0.f, g_pitchPerInput = 0.f;
        int g_calibStep = 0;           // 0 idle, 1 yaw sent, 2 pitch sent, 3 done
        int g_calibYaw0 = 0, g_calibPitch0 = 0;
        float g_calibProbe = 1.f;
        bool g_calibrated = false;

        uintptr_t g_target = 0;
        float g_targetUnseenS = 0.f;
        bool g_firing = false;
        bool g_aimOn = false;
        Axes g_axes;

        const Enemy* FindEnemy(uintptr_t id) {
            for (const Enemy& e : g_enemies) if (e.id == id) return &e;
            return nullptr;
        }

        void Exec(const wchar_t* c) { Engine::ExecConsoleCommand(c); }

        void SetFiring(bool on) {
            if (on == g_firing) return;
            g_firing = on;
            Exec(on ? L"StartFire" : L"StopFire");
            if (on) g_stats.shots++;
        }

        // Returns true while still calibrating (axes are the probe, aim must not run).
        bool Calibrate(APoplarPlayerController* pc) {
            if (g_calibrated) return false;
            switch (g_calibStep) {
            case 0:
                g_calibYaw0 = pc->Rotation.Yaw;
                g_axes = Axes{}; g_axes.forward = 0.f; g_axes.turn = g_calibProbe; g_axes.valid = true;
                g_calibStep = 1;
                return true;
            case 1: {
                int d = WrapUnits(pc->Rotation.Yaw - g_calibYaw0);
                if (std::abs(d) < 2 && g_calibProbe < 100.f) { g_calibProbe *= 10.f; g_calibStep = 0; return true; }
                g_yawPerInput = d / g_calibProbe;
                g_calibPitch0 = pc->Rotation.Pitch;
                g_axes = Axes{}; g_axes.forward = 0.f; g_axes.lookUp = g_calibProbe; g_axes.valid = true;
                g_calibStep = 2;
                return true;
            }
            case 2: {
                int d = WrapUnits(pc->Rotation.Pitch - g_calibPitch0);
                g_pitchPerInput = d != 0 ? d / g_calibProbe : g_yawPerInput;
                g_calibrated = true;
                if (std::abs(g_yawPerInput) < 0.01f) g_yawPerInput = 100.f;   // never divide by zero
                std::printf("[COMBAT] calibrated: yaw %.2f units per input, pitch %.2f (probe %.0f)\n",
                            g_yawPerInput, g_pitchPerInput, g_calibProbe);
                return false;
            }
            default:
                return false;
            }
        }

        // Choose/keep the target. The order's explicit target wins while it is alive and known.
        void PickTarget(float dt, uintptr_t wanted) {
            const Enemy* cur = FindEnemy(g_target);
            if (wanted && FindEnemy(wanted)) { g_target = wanted; g_targetUnseenS = 0.f; return; }
            if (cur && cur->visible) { g_targetUnseenS = 0.f; return; }
            if (cur) { g_targetUnseenS += dt; if (g_targetUnseenS < g_tuning.targetMemoryS) return; }
            g_target = 0; g_targetUnseenS = 0.f;
            float best = 1e12f;
            for (const Enemy& e : g_enemies) {
                if (!e.visible) continue;
                float score = e.distance * (e.kind == "player" ? g_tuning.playerWeight : 1.f);
                if (score < best) { best = score; g_target = e.id; }
            }
            if (g_target) std::printf("[COMBAT] target %llx (%s, %.0f units)\n",
                                      (unsigned long long)g_target, FindEnemy(g_target)->hero.c_str(), best);
        }

        // Turn/look toward the target: one-frame deadbeat step, capped at maxTurnDegPerTick.
        void Aim(const Enemy& e) {
            float maxUnits = g_tuning.maxTurnDegPerTick * kUnitsPerDeg;
            float yawErr = e.bearingDeg * kUnitsPerDeg;
            float pitchErr = e.pitchDeg * kUnitsPerDeg - (float)WrapUnits(g_myPitch);
            yawErr = std::max(-maxUnits, std::min(maxUnits, yawErr));
            pitchErr = std::max(-maxUnits, std::min(maxUnits, pitchErr));
            g_axes.turn = yawErr / g_yawPerInput;
            g_axes.lookUp = pitchErr / g_pitchPerInput;
            g_aimOn = std::fabs(e.bearingDeg) < g_tuning.aimOnDeg &&
                      std::fabs(e.pitchDeg - WrapUnits(g_myPitch) / kUnitsPerDeg) < g_tuning.aimOnDeg;
        }
```
Replace `Combat::Tick` with:
```cpp
    void Tick(float dt, APoplarPlayerController* pc) {
        g_pc = pc;
        APawn* me = pc->Pawn;
        g_myLoc[0] = me->Location.X; g_myLoc[1] = me->Location.Y; g_myLoc[2] = me->Location.Z;
        g_myYaw = pc->Rotation.Yaw; g_myPitch = pc->Rotation.Pitch;
        g_health = me->GetHealth(); g_maxHealth = me->GetMaxHealth();
        g_hero = PawnUtils::HeroOf(me);
        TrackStats(dt, pc);
        if (Calibrate(pc)) return;
        g_sinceCensus += dt;
        if (g_sinceCensus >= kCensusEvery) { g_sinceCensus = 0.f; Census(pc); }

        g_axes = Axes{}; g_axes.valid = true; g_axes.forward = 0.f;
        PickTarget(dt, 0);
        const Enemy* t = FindEnemy(g_target);
        g_aimOn = false;
        if (t) { Aim(*t); g_stats.withTargetS += dt; }
        bool fire = t && g_aimOn && t->visible && t->distance < g_tuning.fireRange;
        SetFiring(fire);
        if (fire) g_stats.firingS += dt;
    }
```
and `OnNoPawn` gains `SetFiring(false); g_target = 0; g_axes = Axes{};` (keep `g_calibrated` — the view input scale does not change on respawn). Add:
```cpp
    Axes CurrentAxes() { return g_axes; }
    Tuning& GetTuning() { return g_tuning; }
```
In `SnapshotJson` set `j["target"]` to the hex id string when `g_target` else `nullptr`, and add `j["aim_on"] = g_aimOn; j["firing"] = g_firing; j["calibrated"] = g_calibrated;`.

- [ ] **Step 3: Let the autopilot write the combat axes**

In `reborn/Autopilot.cpp`, `WriteAxes` becomes:
```cpp
        void WriteAxes(APoplarPlayerController* pc) {
            UPlayerInput* in = pc->PlayerInput;
            Combat::Axes c = Combat::Enabled() ? Combat::CurrentAxes() : Combat::Axes{};
            if (c.valid) {
                in->aBaseY = c.forward;
                in->aStrafe = c.strafe;
                in->aTurn = g_unstickFor > 0.f ? 1.0f : c.turn;
                in->aLookUp = c.lookUp;
                return;
            }
            in->aBaseY = 1.0f;
            in->aStrafe = g_strafe;
            in->aTurn = g_unstickFor > 0.f ? 1.0f : g_turn;
            in->aLookUp = 0.f;
        }
```
In `PlayTick`, when `Combat::Enabled()`, skip the wander's own fire cycle (the `g_fireCycle` block) so the two do not fight over StartFire/StopFire: wrap that block in `if (!Combat::Enabled()) { ... }`. The random turn/strafe plan and jump timer stay (Task 3 uses them as the "wander" mode).

- [ ] **Step 4: Build** — `python -m debugloop.loop build` → `BUILD OK`.

- [ ] **Step 5: Live check (aim)**

Same s0 smoke with `-rbcombat` as Task 1 Step 9. Watch `debugloop/runs/<run>/solo.log` for `[COMBAT] calibrated` (yaw units per input must be non-zero and not the 100 fallback; if it is the fallback, the probe was not consumed — check that `WriteAxes` runs after `Combat::Tick` in the same tick) and `[COMBAT] target`. Poll `/combat` every 2 s with:
```bash
for i in $(seq 1 10); do curl -s http://127.0.0.1:18080/combat | python -c "import json,sys; j=json.load(sys.stdin); t=j['target']; e=[x for x in j['enemies'] if x['id']==t]; print(j['calibrated'], j['aim_on'], j['firing'], e[0]['bearing'] if e else None, e[0]['pitch'] if e else None)"; sleep 2; done
```
Expected: once a target exists, `bearing` and `pitch` go to within ±4° within a second and stay; `firing` is true while visible and under 3000 units. If the bearing oscillates or grows, the sign of `g_yawPerInput` is wrong for the direction convention: flip the sign of `yawErr` (and the same for pitch) and rebuild. If it converges slowly, raise `maxTurnDegPerTick`. Note the final numbers in `docs/notes/combat-census-notes.md`.

- [ ] **Step 6: Commit**

```bash
git add reborn/Combat.hpp reborn/Combat.cpp reborn/Autopilot.cpp docs/notes/combat-census-notes.md
git commit -m "feat(combat): self-calibrating aim, target choice and fire"
```

---

### Task 3: Modes (hunt / hold / goto / follow / retreat / wander), movement, skills

**Files:**
- Modify: `reborn/Combat.hpp`, `reborn/Combat.cpp`, `reborn/Autopilot.cpp` (PlayTick)

**Interfaces:**
- Produces:
  ```cpp
  struct Order { std::string mode = "hunt"; uintptr_t target = 0; bool hasPoint = false; float point[3] = {0,0,0}; bool fire = true; bool skills = true; };
  const Order& CurrentOrder();
  std::string SetOrder(const Order& o);   // returns "" or an error word: unknown_mode
  const char* ModeName();
  ```
  (`/order` JSON parsing is Task 4; this task only provides the C++ API.)

- [ ] **Step 1: Add the order API to `Combat.hpp`** (block above) and `enum class Mode { Hunt, Hold, Goto, Follow, Retreat, Wander };` in the .cpp.

- [ ] **Step 2: Movement + skills in `Combat.cpp`**

Add to the anonymous namespace:
```cpp
        Order g_order;
        Mode g_mode = Mode::Hunt;
        float g_modeTime = 0.f;
        Mode g_resumeMode = Mode::Hunt;     // where auto-retreat returns to
        float g_strafeSign = 1.f, g_untilStrafeFlip = 0.f;
        float g_sinceSkill = 0.f;
        std::mt19937 g_rng{ 7u };

        bool ParseMode(const std::string& s, Mode& m) {
            if (s == "hunt") m = Mode::Hunt; else if (s == "hold") m = Mode::Hold;
            else if (s == "goto") m = Mode::Goto; else if (s == "follow") m = Mode::Follow;
            else if (s == "retreat") m = Mode::Retreat; else if (s == "wander") m = Mode::Wander;
            else return false;
            return true;
        }

        // Forward/strafe that walks toward (or away from) a world point. The view may not face
        // it, so split the direction into the pawn's forward and right axes.
        void WalkToward(const float* pt, float sign, float stopWithin) {
            float dx = pt[0] - g_myLoc[0], dy = pt[1] - g_myLoc[1];
            float dist = std::sqrt(dx * dx + dy * dy);
            if (dist < stopWithin) { g_axes.forward = 0.f; g_axes.strafe = 0.f; return; }
            float yaw = g_myYaw / kUnitsPerDeg * kPi / 180.f;
            float fx = std::cos(yaw), fy = std::sin(yaw);     // view forward
            float rx = -fy, ry = fx;                           // view right (UE3 left-handed, Z up)
            g_axes.forward = sign * (dx * fx + dy * fy) / dist;
            g_axes.strafe = sign * (dx * rx + dy * ry) / dist;
        }

        void TurnToward(const float* pt) {   // when there is no target: face the walking goal
            float dx = pt[0] - g_myLoc[0], dy = pt[1] - g_myLoc[1];
            int yawTo = (int)std::lround(std::atan2(dy, dx) * 180.f / kPi * kUnitsPerDeg);
            float err = (float)WrapUnits(yawTo - g_myYaw);
            float maxUnits = g_tuning.maxTurnDegPerTick * kUnitsPerDeg;
            g_axes.turn = std::max(-maxUnits, std::min(maxUnits, err)) / g_yawPerInput;
        }

        void UseSkills(float dt, APoplarPlayerController* pc, const Enemy* t) {
            g_sinceSkill += dt;
            if (!g_order.skills || !t || !t->visible || g_sinceSkill < 2.f) return;
            int near = 0;
            for (const Enemy& e : g_enemies) if (e.distance < 1500.f) near++;
            EActionSkillSlot slots[3] = { EActionSkillSlot::ASS_SlotOne, EActionSkillSlot::ASS_SlotTwo, EActionSkillSlot::ASS_SlotThree };
            for (int i = 0; i < 3; i++) {
                if (i == 2 && near < 2) continue;                       // ultimate: 2+ enemies near
                if (pc->GetActionSkillSlotCooldownTimeRemaining(slots[i]) > 0.f) continue;
                pc->StartActionSkillBySlot(slots[i]);
                g_stats.skills++; g_sinceSkill = 0.f;
                std::printf("[COMBAT] skill slot %d\n", i + 1);
                return;
            }
        }

        void SetMode(Mode m) { if (m != g_mode) { g_mode = m; g_modeTime = 0.f; std::printf("[COMBAT] mode %s\n", ModeName()); } }
```
(`ModeName` is defined in the public section below; declare `const char* ModeName();` before the namespace block.)

Replace the body of `Tick` after the census with:
```cpp
        g_axes = Axes{}; g_axes.valid = true; g_axes.forward = 0.f;
        g_modeTime += dt;
        PickTarget(dt, g_order.target);
        const Enemy* t = FindEnemy(g_target);
        g_aimOn = false;
        if (t) { Aim(*t); g_stats.withTargetS += dt; }

        // Auto-retreat: low health with an enemy close, in hunt only.
        if (g_mode == Mode::Hunt && g_maxHealth > 0.f && g_health < g_maxHealth * g_tuning.retreatHealthFrac &&
            !g_enemies.empty() && g_enemies[0].distance < g_tuning.retreatNearUnits) {
            g_resumeMode = Mode::Hunt; SetMode(Mode::Retreat);
        }

        switch (g_mode) {
        case Mode::Hunt:
            if (t) {
                WalkToward(t->loc, 1.f, g_tuning.engageRange);
                g_untilStrafeFlip -= dt;
                if (g_untilStrafeFlip <= 0.f) { g_strafeSign = -g_strafeSign; g_untilStrafeFlip = std::uniform_real_distribution<float>(1.5f, 3.f)(g_rng); }
                if (t->distance <= g_tuning.engageRange) g_axes.strafe = g_strafeSign * 0.7f;
            } else {
                g_axes.valid = false;   // no target: the autopilot's wander moves us
            }
            break;
        case Mode::Hold:
            break;
        case Mode::Goto:
            if (!g_order.hasPoint) { SetMode(Mode::Hold); break; }
            WalkToward(g_order.point, 1.f, 150.f);
            if (!t) TurnToward(g_order.point);
            if (g_axes.forward == 0.f && g_axes.strafe == 0.f) SetMode(Mode::Hold);
            break;
        case Mode::Follow: {
            const Enemy* f = FindEnemy(g_order.target);
            if (!f) { SetMode(Mode::Hold); break; }
            if (f->distance > 1200.f) WalkToward(f->loc, 1.f, 1200.f);
            else if (f->distance < 600.f) WalkToward(f->loc, -1.f, 0.f);
            if (!t) TurnToward(f->loc);
            break;
        }
        case Mode::Retreat:
            if (!g_enemies.empty()) WalkToward(g_enemies[0].loc, -1.f, 0.f);
            if (g_modeTime > 5.f) SetMode(g_resumeMode == Mode::Retreat ? Mode::Hold : g_resumeMode);
            break;
        case Mode::Wander:
            g_axes.valid = false;
            break;
        }

        bool fire = g_order.fire && t && g_aimOn && t->visible && t->distance < g_tuning.fireRange;
        SetFiring(fire);
        if (fire) g_stats.firingS += dt;
        UseSkills(dt, pc, t);
```
Public additions:
```cpp
    const Order& CurrentOrder() { return g_order; }

    std::string SetOrder(const Order& o) {
        Mode m;
        if (!ParseMode(o.mode, m)) return "unknown_mode";
        g_order = o;
        g_resumeMode = m == Mode::Retreat ? Mode::Hold : m;
        SetMode(m);
        return "";
    }

    const char* ModeName() {
        switch (g_mode) {
        case Mode::Hunt: return "hunt"; case Mode::Hold: return "hold"; case Mode::Goto: return "goto";
        case Mode::Follow: return "follow"; case Mode::Retreat: return "retreat"; default: return "wander";
        }
    }
```
`SnapshotJson`: `j["mode"] = ModeName();` and
```cpp
        j["order"] = { {"mode", g_order.mode}, {"fire", g_order.fire}, {"skills", g_order.skills},
                       {"target", g_order.target ? nlohmann::json(HexId(g_order.target)) : nlohmann::json(nullptr)},
                       {"point", g_order.hasPoint ? nlohmann::json({g_order.point[0], g_order.point[1], g_order.point[2]}) : nlohmann::json(nullptr)} };
        nlohmann::json skills = nlohmann::json::array();
        if (g_pc && !Gone(g_pc)) for (int i = 0; i < 3; i++) {
            float cd = g_pc->GetActionSkillSlotCooldownTimeRemaining((EActionSkillSlot)i);
            skills.push_back({ {"slot", i + 1}, {"ready", cd <= 0.f}, {"cooldown_s", cd} });
        }
        j["me"]["skills"] = skills;
```
with a helper `std::string HexId(uintptr_t v)` (snprintf `%llx`) replacing the inline snprintf from Task 1. Note the `/combat` handler runs on the game thread (RunOnGameThread), so reading cooldowns there is safe.

- [ ] **Step 3: Make the autopilot's wander the fallback only**

In `Autopilot::PlayTick`, the random plan/jump block runs always (it is harmless when combat axes are valid, since `WriteAxes` prefers them). Keep it. No change beyond Task 2's fire-cycle guard.

- [ ] **Step 4: Build** → `BUILD OK`.

- [ ] **Step 5: Live check (hunt)**

Solo s0 smoke with `-rbcombat` again. Expected in `solo.log`: `[COMBAT] mode hunt` is not printed (hunt is the initial mode; no change), `[COMBAT] target ...` lines, `[COMBAT] skill slot N` lines within the first minute of a fight, and `/combat` shows `stats.shots > 0` by the end. Watch the screenshot in `runs/<run>/screens/` for the player facing a bot. Record which slot fired the ultimate (the game's HUD or the log) in `docs/notes/combat-census-notes.md`; if slot 3 is not the ultimate, change the `i == 2` guard to the right slot.

- [ ] **Step 6: Commit**

```bash
git add reborn/Combat.hpp reborn/Combat.cpp reborn/Autopilot.cpp docs/notes/combat-census-notes.md
git commit -m "feat(combat): hunt/hold/goto/follow/retreat modes and skill use"
```

---

### Task 4: `/order` and `/act` routes

**Files:**
- Modify: `reborn/Combat.hpp`, `reborn/Combat.cpp`, `reborn/dllmain.cpp` (the two placeholder lambdas)

**Interfaces:**
- Produces: `std::string Combat::ApplyOrderJson(const std::string& body);` and `std::string Combat::ActJson(const std::string& body);` — JSON strings; errors are `{"error":"<word>","status":NNN}` (status 400 bad_json/unknown_mode/unknown_action/bad_point, 409 no_pawn). Also `/order` accepts `"enabled": bool` and `"tuning": {name: number}`.

- [ ] **Step 1: Implement in `Combat.cpp`**

```cpp
    namespace {
        std::string Err(const char* word, int status) {
            return std::string("{\"error\":\"") + word + "\",\"status\":" + std::to_string(status) + "}";
        }
        uintptr_t ParseId(const nlohmann::json& v) {
            if (!v.is_string()) return 0;
            return (uintptr_t)std::strtoull(v.get<std::string>().c_str(), nullptr, 16);
        }
        float g_burstLeft = 0.f;   // fire_burst: seconds of forced fire remaining
    }

    std::string ApplyOrderJson(const std::string& body) {
        nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
        if (j.is_discarded() || !j.is_object()) return Err("bad_json", 400);
        Order o = g_order;
        if (j.contains("mode")) { if (!j["mode"].is_string()) return Err("unknown_mode", 400); o.mode = j["mode"]; }
        if (j.contains("target")) o.target = j["target"].is_null() ? 0 : ParseId(j["target"]);
        if (j.contains("point")) {
            if (j["point"].is_null()) o.hasPoint = false;
            else if (j["point"].is_array() && j["point"].size() == 3) { o.hasPoint = true; for (int i = 0; i < 3; i++) o.point[i] = j["point"][i].get<float>(); }
            else return Err("bad_point", 400);
        }
        if (j.contains("fire")) o.fire = j["fire"].get<bool>();
        if (j.contains("skills")) o.skills = j["skills"].get<bool>();
        if (j.contains("tuning") && j["tuning"].is_object()) {
            Tuning& t = g_tuning;
            for (auto& [k, v] : j["tuning"].items()) {
                float f = v.get<float>();
                if (k == "fireRange") t.fireRange = f; else if (k == "engageRange") t.engageRange = f;
                else if (k == "aimOnDeg") t.aimOnDeg = f; else if (k == "maxTurnDegPerTick") t.maxTurnDegPerTick = f;
                else if (k == "retreatHealthFrac") t.retreatHealthFrac = f; else if (k == "retreatNearUnits") t.retreatNearUnits = f;
                else if (k == "playerWeight") t.playerWeight = f; else if (k == "targetMemoryS") t.targetMemoryS = f;
            }
        }
        std::string err = SetOrder(o);
        if (!err.empty()) return Err(err.c_str(), 400);
        if (j.contains("enabled")) SetEnabled(j["enabled"].get<bool>());
        return SnapshotJson();
    }

    std::string ActJson(const std::string& body) {
        nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
        if (j.is_discarded() || !j.is_object() || !j["action"].is_string()) return Err("bad_json", 400);
        std::string a = j["action"];
        float dur = j.value("duration_s", 0.5f);
        APoplarPlayerController* pc = SDKUtils::GetLocalPlayerController();
        if (!pc || Gone(pc->Pawn)) return Err("no_pawn", 409);
        if (a == "jump") Exec(L"Jump");
        else if (a == "fire_start") { g_order.fire = true; SetFiring(true); }
        else if (a == "fire_stop") { SetFiring(false); g_order.fire = false; }
        else if (a == "fire_burst") { g_burstLeft = dur; SetFiring(true); }
        else if (a == "altfire") { Exec(L"StartAltFire"); Exec(L"StopAltFire"); }
        else if (a == "melee") { Exec(L"StartOffHandFire"); Exec(L"StopOffHandFire"); }
        else if (a == "skill1") pc->StartActionSkillBySlot(EActionSkillSlot::ASS_SlotOne);
        else if (a == "skill2") pc->StartActionSkillBySlot(EActionSkillSlot::ASS_SlotTwo);
        else if (a == "ultimate") pc->StartActionSkillBySlot(EActionSkillSlot::ASS_SlotThree);
        else if (a == "use") pc->Use();
        else if (a == "sprint_start") pc->BeginSprint();
        else if (a == "sprint_stop") pc->EndSprint();
        else return Err("unknown_action", 400);
        return "{\"ok\":true,\"action\":\"" + a + "\"}";
    }
```
In `Tick`, before `SetFiring(fire)`: `if (g_burstLeft > 0.f) { g_burstLeft -= dt; fire = true; }`. Add `#include <cstdlib>`. Declare both functions in `Combat.hpp`.

- [ ] **Step 2: Wire in `dllmain.cpp`**

Replace the two `not_implemented` lambdas with `Combat::ApplyOrderJson` and `Combat::ActJson`.

- [ ] **Step 3: Build** → `BUILD OK`.

- [ ] **Step 4: Live check (orders)**

Solo s0 smoke with `-rbcombat`. From a shell:
```bash
curl -s -X POST http://127.0.0.1:18080/order -d '{"mode":"hold"}' | head -c 300; echo
curl -s -X POST http://127.0.0.1:18080/order -d '{"mode":"dance"}'; echo
curl -s -X POST http://127.0.0.1:18080/order -d 'not json'; echo
curl -s -X POST http://127.0.0.1:18080/act -d '{"action":"jump"}'; echo
curl -s -X POST http://127.0.0.1:18080/act -d '{"action":"teabag"}'; echo
curl -s -X POST http://127.0.0.1:18080/order -d '{"mode":"hunt"}' | head -c 100; echo
```
Expected: 1st prints the snapshot with `"mode":"hold"`; 2nd `{"error":"unknown_mode","status":400}` (and `curl -i` shows `HTTP/1.1 400`); 3rd `bad_json`; 4th `{"ok":true,...}` and the player jumps on screen; 5th `unknown_action`; 6th back to hunt. Also `[COMBAT] mode hold` / `mode hunt` in `solo.log`.

- [ ] **Step 5: Commit**

```bash
git add reborn/Combat.hpp reborn/Combat.cpp reborn/dllmain.cpp
git commit -m "feat(combat): /order and /act routes"
```

---

### Task 5: Fake game grows the three routes (test double for Python work)

**Files:**
- Modify: `debugloop/tests/fake_game.py`

**Interfaces:**
- Produces: the fake answers `GET /combat`, `POST /order`, `POST /act`, `GET /ping`, `GET /log` with the same JSON contract as Tasks 1-4, and `--mode=dead` makes `/act` answer 409 `no_pawn`. State persists in-process so an order round-trips.

- [ ] **Step 1: Extend the fake**

Replace the `H` class in `fake_game.py` with:
```python
    order = {"mode": "hunt", "target": None, "point": None, "fire": True, "skills": True}
    MODES = {"hunt", "hold", "goto", "follow", "retreat", "wander"}
    ACTIONS = {"jump", "fire_start", "fire_stop", "fire_burst", "altfire", "melee", "skill1", "skill2",
               "ultimate", "use", "sprint_start", "sprint_stop"}

    def combat():
        return {"enabled": True, "mode": order["mode"], "calibrated": True, "aim_on": False, "firing": False,
                "me": {"hero": "Rath", "location": [10, 20, 30], "yaw": 90.0, "pitch": 0.0, "health": 800,
                       "max_health": 1000, "dead": mode == "dead",
                       "skills": [{"slot": 1, "ready": True, "cooldown_s": 0}, {"slot": 2, "ready": False, "cooldown_s": 4.5},
                                  {"slot": 3, "ready": False, "cooldown_s": 60}]},
                "target": "7f0001", "order": order,
                "enemies": [{"id": "7f0001", "hero": "Thrall", "kind": "minion", "distance": 640.0, "bearing": -12.5,
                             "pitch": 2.0, "visible": True, "health": 300, "team": 1, "location": [600, 40, 30]},
                            {"id": "7f0002", "hero": "OscarMike", "kind": "bot", "distance": 2100.0, "bearing": 95.0,
                             "pitch": -1.0, "visible": False, "health": 900, "team": 1, "location": [-100, 2000, 30]}],
                "n_enemies": 2,
                "stats": {"shots": 3, "skills_used": 1, "kills": 0, "deaths": 1, "damage_taken": 200.0,
                          "time_with_target_s": 12.0, "time_firing_s": 4.0}}

    class H(BaseHTTPRequestHandler):
        def _json(self, obj, status=200):
            body = json.dumps(obj).encode()
            self.send_response(status); self.send_header("Content-Type", "application/json")
            self.end_headers(); self.wfile.write(body)

        def do_GET(self):
            if mode == "hang" and time.time() - start > 2:
                self.send_response(503); self.end_headers()
                self.wfile.write(b'{"error":"game_thread_unresponsive"}')
                return
            if self.path == "/ping":
                self.send_response(200); self.end_headers(); self.wfile.write(b"pong"); return
            if self.path == "/log":
                self.send_response(200); self.end_headers()
                self.wfile.write(b"[GAME] line one\n[COMBAT] target 7f0001\n"); return
            if self.path == "/combat":
                return self._json(combat())
            self._json(snapshot())

        def do_POST(self):
            n = int(self.headers.get("Content-Length") or 0)
            raw = self.rfile.read(n).decode("utf-8", "replace")
            if self.path == "/exec":
                self.send_response(200); self.end_headers(); self.wfile.write(b"ok"); return
            try:
                j = json.loads(raw)
                if not isinstance(j, dict):
                    raise ValueError
            except ValueError:
                return self._json({"error": "bad_json", "status": 400}, 400)
            if self.path == "/order":
                if "mode" in j and j["mode"] not in MODES:
                    return self._json({"error": "unknown_mode", "status": 400}, 400)
                for k in ("mode", "target", "point", "fire", "skills"):
                    if k in j:
                        order[k] = j[k]
                return self._json(combat())
            if self.path == "/act":
                if mode == "dead":
                    return self._json({"error": "no_pawn", "status": 409}, 409)
                if j.get("action") not in ACTIONS:
                    return self._json({"error": "unknown_action", "status": 400}, 400)
                return self._json({"ok": True, "action": j["action"]})
            self._json({"error": "not_found", "status": 404}, 404)

        def log_message(self, *_):
            pass
```
Also add `"instance": name, "role": role, "pid": os.getpid()` to both dicts returned by `snapshot()` (the discovery code in Task 6 reads them).

- [ ] **Step 2: Run the existing tests** — `python -m pytest debugloop/tests -q`. Expected: all pass (the fake still answers `/state` as before).

- [ ] **Step 3: Commit**

```bash
git add debugloop/tests/fake_game.py
git commit -m "test(debugloop): fake game answers /combat, /order and /act"
```

---

### Task 6: `debugloop/play.py` — client library and CLI

**Files:**
- Create: `debugloop/play.py`
- Test: `debugloop/tests/test_play.py`

**Interfaces:**
- Produces:
  ```python
  class PlayError(Exception): ...
  @dataclass
  class Instance: name: str; role: str; port: int; pid: int
  def discover(ports=range(18080, 18090), timeout=0.3) -> list[Instance]
  def resolve(which: str | int, instances=None) -> Instance     # by name or port; PlayError if absent
  def state(inst) -> dict;  def combat(inst) -> dict
  def order(inst, mode=None, target=None, point=None, fire=None, skills=None, enabled=None, tuning=None) -> dict
  def act(inst, action: str, duration_s: float | None = None) -> dict
  def exec_(inst, command: str) -> str;  def log(inst, lines: int = 40) -> str
  def look(inst, max_width: int = 480) -> bytes | None          # PNG
  def situation_text(inst) -> str
  def main(argv=None) -> int
  ```
  Every HTTP error becomes `PlayError(f"{inst.name}: {error word}")`.

- [ ] **Step 1: Write the failing tests**

`debugloop/tests/test_play.py`:
```python
import json
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

import pytest

from debugloop import play

FAKE = Path(__file__).with_name("fake_game.py")


@pytest.fixture
def fake(tmp_path):
    """One fake client on port 18180 (outside the harness's range so nothing collides)."""
    procs = []

    def start(port=18180, mode="pass", name="c1"):
        p = subprocess.Popen([sys.executable, str(FAKE), f"-rbinstance={name}", f"-rbdebugport={port}",
                              f"-rbrundir={tmp_path}", "--role=client", f"--mode={mode}"])
        procs.append(p)
        for _ in range(50):
            try:
                urllib.request.urlopen(f"http://127.0.0.1:{port}/ping", timeout=0.2)
                break
            except OSError:
                time.sleep(0.1)
        return play.Instance(name, "client", port, p.pid)

    yield start
    for p in procs:
        p.kill()


def test_discover_finds_the_fake(fake):
    fake()
    found = play.discover(ports=range(18180, 18182))
    assert [(i.name, i.role, i.port) for i in found] == [("c1", "client", 18180)]


def test_resolve_by_name_and_port(fake):
    inst = fake()
    assert play.resolve("c1", [inst]).port == 18180
    assert play.resolve(18180, [inst]).name == "c1"
    with pytest.raises(play.PlayError, match="no instance"):
        play.resolve("c9", [inst])


def test_order_round_trips(fake):
    inst = fake()
    j = play.order(inst, mode="hold", fire=False)
    assert j["mode"] == "hold" and j["order"]["fire"] is False


def test_order_rejects_unknown_mode(fake):
    inst = fake()
    with pytest.raises(play.PlayError, match="unknown_mode"):
        play.order(inst, mode="dance")


def test_act_without_pawn_is_a_plain_error(fake):
    inst = fake(mode="dead")
    with pytest.raises(play.PlayError, match="no_pawn"):
        play.act(inst, "jump")


def test_act_ok(fake):
    assert play.act(fake(), "jump") == {"ok": True, "action": "jump"}


def test_situation_text_is_short_and_names_the_target(fake):
    text = play.situation_text(fake())
    lines = text.splitlines()
    assert len(lines) <= 20
    assert "Rath" in text and "800/1000" in text and "hold" not in text
    assert "* 7f0001 Thrall minion 640u bearing -12 visible hp 300" in text
    assert "skills: 1 ready, 2 in 4.5s, 3 in 60.0s" in text


def test_look_without_window_returns_none(fake, monkeypatch):
    inst = fake()
    monkeypatch.setattr(play.screenshot, "capture", lambda pid, path: False)
    assert play.look(inst) is None


def test_look_resizes_png(fake, monkeypatch, tmp_path):
    from PIL import Image

    def fake_capture(pid, path):
        Image.new("RGB", (960, 540), "red").save(path)
        return True

    monkeypatch.setattr(play.screenshot, "capture", fake_capture)
    data = play.look(fake(), max_width=240)
    img = Image.open(__import__("io").BytesIO(data))
    assert img.format == "PNG" and img.size == (240, 135)


def test_cli_list_and_situation(fake, capsys):
    fake()
    assert play.main(["--ports", "18180-18181", "list"]) == 0
    assert "c1" in capsys.readouterr().out
    assert play.main(["--ports", "18180-18181", "situation", "c1"]) == 0
    assert "Thrall" in capsys.readouterr().out


def test_cli_error_is_one_line(fake, capsys):
    fake(mode="dead")
    assert play.main(["--ports", "18180-18181", "act", "c1", "jump"]) == 1
    assert "no_pawn" in capsys.readouterr().out
```

- [ ] **Step 2: Run them** — `python -m pytest debugloop/tests/test_play.py -q`. Expected: ImportError / failures (no `play` module).

- [ ] **Step 3: Write `debugloop/play.py`**

```python
"""Talk to a running test game: read its combat situation, give orders, take a small picture.

Library for the MCP server and a CLI (`python -m debugloop.play ...`). Only ever talks to
127.0.0.1. Never starts or stops a game.
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


def _request(inst: Instance, path: str, body: dict | str | None = None, timeout: float = 6.0):
    url = f"http://127.0.0.1:{inst.port}{path}"
    data = None
    if body is not None:
        data = (body if isinstance(body, str) else json.dumps(body)).encode()
    req = urllib.request.Request(url, data=data, method="POST" if data is not None else "GET")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        raw = e.read().decode("utf-8", "replace")
        try:
            word = json.loads(raw).get("error", raw)
        except ValueError:
            word = raw or str(e.code)
        raise PlayError(f"{inst.name}: {word}") from None
    except (urllib.error.URLError, TimeoutError, ConnectionError, http.client.HTTPException) as e:
        raise PlayError(f"{inst.name}: no answer from port {inst.port} ({e})") from None


def _json(inst: Instance, path: str, body=None) -> dict:
    try:
        return json.loads(_request(inst, path, body))
    except ValueError:
        raise PlayError(f"{inst.name}: {path} did not return JSON") from None


def discover(ports=DEFAULT_PORTS, timeout: float = 0.3) -> list[Instance]:
    found = []
    for port in ports:
        probe = Instance(f"port {port}", "?", port, 0)
        try:
            st = _json(probe, "/state")
        except PlayError:
            continue
        found.append(Instance(str(st.get("instance", probe.name)), str(st.get("role", "?")), port,
                              int(st.get("pid", 0))))
    return found


def resolve(which, instances: list[Instance] | None = None) -> Instance:
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
    body = {k: v for k, v in {"mode": mode, "target": target, "point": point, "fire": fire,
                              "skills": skills, "enabled": enabled, "tuning": tuning}.items() if v is not None}
    return _json(inst, "/order", body)


def act(inst: Instance, action: str, duration_s: float | None = None) -> dict:
    body = {"action": action}
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
        img = Image.open(p)
        img.load()
    if img.width > max_width:
        img = img.resize((max_width, max(1, round(img.height * max_width / img.width))))
    out = io.BytesIO()
    img.save(out, "PNG")
    return out.getvalue()


def situation_text(inst: Instance) -> str:
    c = combat(inst)
    me = c.get("me", {})
    sk = ", ".join(f"{s['slot']} ready" if s.get("ready") else f"{s['slot']} in {s.get('cooldown_s', 0):.1f}s"
                   for s in me.get("skills", []))
    loc = me.get("location") or [0, 0, 0]
    lines = [
        f"{inst.name}: {me.get('hero', '?')} hp {me.get('health', 0):.0f}/{me.get('max_health', 0):.0f}"
        f"{' DEAD' if me.get('dead') else ''} at ({loc[0]:.0f}, {loc[1]:.0f}, {loc[2]:.0f}) yaw {me.get('yaw', 0):.0f}",
        f"mode {c.get('mode')}  target {c.get('target') or 'none'}  aim {'on' if c.get('aim_on') else 'off'}"
        f"  firing {'yes' if c.get('firing') else 'no'}  combat {'on' if c.get('enabled') else 'OFF'}",
        f"skills: {sk or 'unknown'}",
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
    o.add_argument("instance"); o.add_argument("mode", choices=MODES)
    o.add_argument("--target"); o.add_argument("--point", nargs=3, type=float)
    o.add_argument("--no-fire", action="store_true"); o.add_argument("--no-skills", action="store_true")
    a = sub.add_parser("act"); a.add_argument("instance"); a.add_argument("action", choices=ACTIONS)
    a.add_argument("--duration", type=float)
    e = sub.add_parser("exec"); e.add_argument("instance"); e.add_argument("command")
    lk = sub.add_parser("look"); lk.add_argument("instance"); lk.add_argument("--out"); lk.add_argument("--width", type=int, default=480)
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
            print(json.dumps(order(inst, mode=ns.mode, target=ns.target, point=ns.point,
                                   fire=False if ns.no_fire else None,
                                   skills=False if ns.no_skills else None)["order"]))
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
```

- [ ] **Step 4: Run the tests** — `python -m pytest debugloop/tests/test_play.py -q`. Expected: all pass. Then the whole suite: `python -m pytest debugloop/tests -q`.

- [ ] **Step 5: Commit**

```bash
git add debugloop/play.py debugloop/tests/test_play.py
git commit -m "feat(debugloop): play client library and CLI for the combat routes"
```

---

### Task 7: MCP server

**Files:**
- Create: `debugloop/mcp_server.py`, `.mcp.json` (repo root `reborn/`)
- Modify: `debugloop/requirements.txt`
- Test: `debugloop/tests/test_mcp_server.py`

**Interfaces:**
- Consumes: everything in `play.py`.
- Produces: tool functions `bb_list`, `bb_state`, `bb_situation`, `bb_combat`, `bb_look`, `bb_order`, `bb_act`, `bb_exec`, `bb_log` as plain module-level functions (testable), registered on a FastMCP server named `battleborn-play` by `build_server()`; `main()` runs stdio.

- [ ] **Step 1: Install the dependency and record it**

```bash
pip install "mcp>=1.2"
```
Append `mcp>=1.2` to `debugloop/requirements.txt`.

- [ ] **Step 2: Write the failing tests**

`debugloop/tests/test_mcp_server.py`:
```python
import pytest

from debugloop import mcp_server, play


class FakePlay:
    """Stands in for debugloop.play inside the tool functions."""

    def __init__(self):
        self.inst = play.Instance("c1", "client", 18080, 123)
        self.calls = []

    def discover(self, *a, **k):
        return [self.inst]

    def resolve(self, which, instances=None):
        if which != "c1":
            raise play.PlayError(f"no instance '{which}'; running: c1 (18080)")
        return self.inst

    def situation_text(self, inst):
        return "c1: Rath hp 800/1000"

    def combat(self, inst):
        return {"mode": "hunt"}

    def state(self, inst):
        return {"map": "Dojo_P"}

    def order(self, inst, **kw):
        self.calls.append(("order", kw))
        return {"order": {"mode": kw.get("mode", "hunt")}}

    def act(self, inst, action, duration_s=None):
        if action == "teabag":
            raise play.PlayError("c1: unknown_action")
        return {"ok": True, "action": action}

    def exec_(self, inst, command):
        return "ok"

    def log(self, inst, lines=40):
        return "[GAME] hi"

    def look(self, inst, max_width=480):
        return None


@pytest.fixture
def fp(monkeypatch):
    f = FakePlay()
    monkeypatch.setattr(mcp_server, "play", f)
    return f


def test_bb_list(fp):
    assert "c1" in mcp_server.bb_list() and "18080" in mcp_server.bb_list()


def test_bb_situation(fp):
    assert mcp_server.bb_situation("c1") == "c1: Rath hp 800/1000"


def test_unknown_instance_is_text_not_exception(fp):
    out = mcp_server.bb_situation("c9")
    assert out.startswith("error:") and "c9" in out


def test_bb_order_passes_fields(fp):
    mcp_server.bb_order("c1", mode="goto", point=[1, 2, 3], fire=False)
    assert fp.calls == [("order", {"mode": "goto", "target": None, "point": [1, 2, 3], "fire": False,
                                   "skills": None, "enabled": None})]


def test_bb_act_error_is_text(fp):
    assert "unknown_action" in mcp_server.bb_act("c1", "teabag")


def test_bb_look_without_window_is_text(fp):
    assert mcp_server.bb_look("c1") == "c1: no window to capture (is the game minimised or gone?)"


def test_bb_look_returns_image(fp, monkeypatch):
    monkeypatch.setattr(fp, "look", lambda inst, max_width=480: b"\x89PNG fake")
    img = mcp_server.bb_look("c1")
    assert img.data == b"\x89PNG fake" and img._format == "png"


def test_build_server_registers_tools():
    srv = mcp_server.build_server()
    names = {t.name for t in srv._tool_manager.list_tools()}
    assert {"bb_list", "bb_situation", "bb_look", "bb_order", "bb_act", "bb_exec", "bb_log", "bb_state",
            "bb_combat"} <= names
```
Note: FastMCP's `Image` keeps the format in `_format`; if the installed version names it differently, read `mcp/server/fastmcp/utilities/types.py` and adjust the single assertion.

- [ ] **Step 3: Run** — `python -m pytest debugloop/tests/test_mcp_server.py -q`. Expected: ImportError.

- [ ] **Step 4: Write `debugloop/mcp_server.py`**

```python
"""MCP server "battleborn-play": lets Claude look at a running test game and give it orders.

Thin wrapper over debugloop.play. Errors come back as text, never as exceptions, so the
transport stays up. Never starts or stops a game (that is `loop play` / `loop stop-play`).
"""
from __future__ import annotations

import json
import sys

from mcp.server.fastmcp import FastMCP
from mcp.server.fastmcp.utilities.types import Image

from . import play


def _guard(fn):
    def run(*a, **k):
        try:
            return fn(*a, **k)
        except play.PlayError as e:
            return f"error: {e}"
    run.__name__, run.__doc__ = fn.__name__, fn.__doc__
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
    """Short text: my hero/health/position, mode, target, skills, known enemies (distance, bearing, visible), stats."""
    return play.situation_text(play.resolve(instance))


@_guard
def bb_combat(instance: str) -> str:
    """Raw /combat JSON (everything bb_situation summarises, plus enemy locations and the current order)."""
    return json.dumps(play.combat(play.resolve(instance)), indent=1)


@_guard
def bb_state(instance: str) -> str:
    """Raw /state JSON: map, role, pawn, match state, memory, combat stats."""
    return json.dumps(play.state(play.resolve(instance)), indent=1)


@_guard
def bb_look(instance: str, max_width: int = 480):
    """A picture of that player's screen, scaled to max_width pixels (default 480; keep it small, pictures cost usage)."""
    inst = play.resolve(instance)
    data = play.look(inst, max_width)
    if data is None:
        return f"{inst.name}: no window to capture (is the game minimised or gone?)"
    return Image(data=data, format="png")


@_guard
def bb_order(instance: str, mode: str | None = None, target: str | None = None,
             point: list[float] | None = None, fire: bool | None = None, skills: bool | None = None,
             enabled: bool | None = None) -> str:
    """Set the standing order. mode: hunt|hold|goto|follow|retreat|wander. target: an enemy id from
    bb_situation (hunt/follow). point: [x,y,z] for goto. fire/skills: allow shooting / abilities.
    enabled: turn the combat brain on or off. Omitted fields keep their value."""
    j = play.order(play.resolve(instance), mode=mode, target=target, point=point, fire=fire,
                   skills=skills, enabled=enabled)
    return json.dumps(j.get("order", j))


@_guard
def bb_act(instance: str, action: str, duration_s: float | None = None) -> str:
    """One-off action: jump, fire_start, fire_stop, fire_burst (duration_s), altfire, melee, skill1, skill2,
    ultimate, use, sprint_start, sprint_stop."""
    return json.dumps(play.act(play.resolve(instance), action, duration_s))


@_guard
def bb_exec(instance: str, command: str) -> str:
    """Send a raw game console command (e.g. "Jump", "StartFire"). Use bb_act when it covers the need."""
    return play.exec_(play.resolve(instance), command)


@_guard
def bb_log(instance: str, lines: int = 40) -> str:
    """The last N lines of that game's log."""
    return play.log(play.resolve(instance), lines)


TOOLS = (bb_list, bb_situation, bb_combat, bb_state, bb_look, bb_order, bb_act, bb_exec, bb_log)


def build_server() -> FastMCP:
    srv = FastMCP("battleborn-play")
    for fn in TOOLS:
        srv.tool()(fn)
    return srv


def main() -> int:
    build_server().run("stdio")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```
If `srv.tool()(fn)` rejects the wrapped function's signature (FastMCP reads annotations from the wrapper), replace `_guard` with `functools.wraps(fn)` on `run`.

- [ ] **Step 5: Register the server**

Create `.mcp.json` at the repo root (`reborn/`):
```json
{
  "mcpServers": {
    "battleborn-play": {
      "command": "python",
      "args": ["-m", "debugloop.mcp_server"],
      "cwd": "C:\\Users\\djsin\\Documents\\GitHub\\Battleborn-Server\\reborn"
    }
  }
}
```

- [ ] **Step 6: Run the tests** — `python -m pytest debugloop/tests -q`. Expected: all pass. Then a smoke of the transport: `python -c "from debugloop import mcp_server; s=mcp_server.build_server(); print(len(s._tool_manager.list_tools()))"` → `9`.

- [ ] **Step 7: Commit**

```bash
git add debugloop/mcp_server.py debugloop/tests/test_mcp_server.py debugloop/requirements.txt .mcp.json
git commit -m "feat(debugloop): battleborn-play MCP server"
```

Tell the user: Claude Code must be restarted from `reborn/` (or the project re-opened) and the new `battleborn-play` server approved once before the `bb_*` tools appear.

---

### Task 8: `loop play` / `loop stop-play` and the guard in `next`/`verify`

**Files:**
- Create: `debugloop/playsession.py`
- Modify: `debugloop/loop.py:372-378, 440-446, 554-560`
- Test: `debugloop/tests/test_playsession.py`, `debugloop/tests/test_loop.py`

**Interfaces:**
- Produces:
  ```python
  # playsession.py
  PLAY_FILE = "play.json"
  def start(scn, launcher=None, runs_dir=config.RUNS_DIR, state_dir=config.STATE_DIR, poll_s=2.0, wait_s=STARTUP_TIMEOUT_S) -> dict   # the play.json content
  def alive(state_dir=config.STATE_DIR) -> dict | None   # play.json content if any of its pids is still our process
  def stop(state_dir=config.STATE_DIR) -> list[str]      # names killed
  ```
  `loop.Deps` gains `play_alive: Callable = playsession.alive`.

- [ ] **Step 1: Failing tests for the session module**

`debugloop/tests/test_playsession.py`:
```python
import json
import subprocess
import sys
import time
from pathlib import Path

import pytest

from debugloop import launch, playsession, run, scenario

FAKE = Path(__file__).with_name("fake_game.py")


class FakeLauncher:
    def __init__(self):
        self.started = []

    def start(self, name, role, args):
        p = subprocess.Popen([sys.executable, str(FAKE), *args, f"--role={role}", "--mode=pass"])
        h = launch.ProcessHandle(p.pid)
        self.started.append(h)
        return h


def scn():
    text = ('name="p"\nstep=1\ntime_limit_s=60\nexpect_map="Dojo_P"\n'
            '[[process]]\nname="server"\nrole="server"\nargs=[]\n'
            '[[process]]\nname="c1"\nrole="client"\nargs=["-rbautopilot"]\n')
    return scenario.parse(text, Path("p.toml"))


@pytest.fixture(autouse=True)
def quiet(monkeypatch, tmp_path):
    monkeypatch.setattr(launch, "find_game_processes", lambda: [])
    monkeypatch.setattr(run.config, "GAME_LOGS_DIR", tmp_path / "no-game-logs")


def test_start_leaves_processes_running_and_writes_play_json(tmp_path):
    L = FakeLauncher()
    info = playsession.start(scn(), launcher=L, runs_dir=tmp_path / "runs", state_dir=tmp_path / "state",
                             poll_s=0.2, wait_s=10)
    try:
        assert set(info["ports"]) == {"server", "c1"}
        assert all(h.exit_code() is None for h in L.started)
        assert (tmp_path / "state" / "play.json").exists()
        assert not (tmp_path / "runs" / "active.json").exists()
        assert playsession.alive(tmp_path / "state") is not None
    finally:
        playsession.stop(tmp_path / "state")


def test_stop_kills_and_forgets(tmp_path):
    L = FakeLauncher()
    playsession.start(scn(), launcher=L, runs_dir=tmp_path / "runs", state_dir=tmp_path / "state",
                      poll_s=0.2, wait_s=10)
    killed = playsession.stop(tmp_path / "state")
    time.sleep(0.5)
    assert sorted(killed) == ["c1", "server"]
    assert all(h.exit_code() is not None for h in L.started)
    assert playsession.alive(tmp_path / "state") is None
    assert playsession.stop(tmp_path / "state") == []


def test_alive_is_none_for_stale_file(tmp_path):
    (tmp_path / "state").mkdir()
    (tmp_path / "state" / "play.json").write_text(json.dumps(
        {"run_dir": "x", "ports": {"c1": 18080}, "pids": {"c1": {"pid": 1, "create_time": 0, "exe": "nope"}}}))
    assert playsession.alive(tmp_path / "state") is None
```
And in `debugloop/tests/test_loop.py` add:
```python
def test_next_refuses_while_play_session_alive(tmp_path, capsys):
    d = deps(tmp_path, [("pass", None, "playing", 100.0)])
    d.play_alive = lambda: {"run_dir": "r", "ports": {"c1": 18081}}
    assert loop.cmd_next(d) == loop.HARNESS
    assert "play session" in capsys.readouterr().out
    assert "run" not in d.log
    assert LoopState.load(tmp_path / "state").harness_errors_in_row == 0
```

- [ ] **Step 2: Run** — expected ImportError / AttributeError.

- [ ] **Step 3: Write `debugloop/playsession.py`**

```python
"""A scenario left running for live driving (the play MCP / CLI), outside the graded loop.

`loop play <scenario>` builds, deploys and starts it, waits for the players to be in the game,
records pids and ports in state/play.json and exits with the games still running.
`loop stop-play` ends them. `loop next`/`verify` refuse while a session is alive.
"""
from __future__ import annotations

import json
import time
from datetime import datetime
from pathlib import Path

from . import config, launch, run
from .outcome import STARTUP_TIMEOUT_S
from .state import atomic_write

PLAY_FILE = "play.json"


def _file(state_dir: Path) -> Path:
    return state_dir / PLAY_FILE


def start(scn, launcher=None, runs_dir: Path = config.RUNS_DIR, state_dir: Path = config.STATE_DIR,
          poll_s: float = 2.0, wait_s: float = STARTUP_TIMEOUT_S) -> dict:
    launcher = launcher or launch.RealLauncher()
    run.preconditions(runs_dir, len(scn.processes))
    run_dir = runs_dir / f"{datetime.now():%Y%m%d-%H%M%S}-play-{scn.name}"
    run_dir.mkdir(parents=True)
    ports = {p.name: config.FIRST_DEBUG_PORT + i for i, p in enumerate(scn.processes)}
    n_clients = sum(p.role == "client" for p in scn.processes)
    handles = {}
    info = {"run_dir": str(run_dir), "scenario": scn.name, "ports": ports, "pids": {}}

    def remember():
        info["pids"] = {n: run._identity(h.pid) or {"pid": h.pid} for n, h in handles.items()}
        state_dir.mkdir(parents=True, exist_ok=True)
        atomic_write(_file(state_dir), json.dumps(info, indent=2))

    try:
        t0 = time.time()
        for spec in [p for p in scn.processes if p.role == "server"]:
            handles[spec.name] = launcher.start(spec.name, spec.role, run._args(spec, ports[spec.name], run_dir, n_clients))
            remember()
            while time.time() - t0 < wait_s:
                st, _ = run._get_state(ports[spec.name])
                if st and st.get("listening"):
                    break
                time.sleep(poll_s)
        for spec in [p for p in scn.processes if p.role != "server"]:
            handles[spec.name] = launcher.start(spec.name, spec.role, run._args(spec, ports[spec.name], run_dir, n_clients))
            remember()
        clients = [p.name for p in scn.processes if p.role != "server"]
        while clients and time.time() - t0 < wait_s:
            states = [run._get_state(ports[n])[0] or {} for n in clients]
            if all(s.get("autopilot") == "playing" or s.get("has_pawn") for s in states):
                break
            time.sleep(poll_s)
    except (RuntimeError, OSError) as e:
        for h in handles.values():
            try:
                h.kill()
            finally:
                h.close()
        _file(state_dir).unlink(missing_ok=True)
        raise run.HarnessError(str(e)) from e
    for h in handles.values():
        h.close()   # the pids are remembered in play.json; stop() reopens them
    return info


def alive(state_dir: Path = config.STATE_DIR) -> dict | None:
    f = _file(state_dir)
    if not f.exists():
        return None
    try:
        info = json.loads(f.read_text(encoding="utf-8"))
    except (ValueError, OSError):
        return None
    if any(run._is_same_process(rec) for rec in info.get("pids", {}).values()):
        return info
    return None


def stop(state_dir: Path = config.STATE_DIR) -> list[str]:
    info = alive(state_dir)
    killed = []
    if info:
        for name, rec in info.get("pids", {}).items():
            if not run._is_same_process(rec):
                continue
            try:
                h = launch.ProcessHandle(int(rec["pid"]))
            except OSError:
                continue
            try:
                h.kill()
                killed.append(name)
            finally:
                h.close()
    _file(state_dir).unlink(missing_ok=True)
    return killed
```
Check `launch.ProcessHandle.close()` exists (line 36-66 of `launch.py`); if the handle has no `close`, drop those calls.

- [ ] **Step 4: Wire the loop**

In `debugloop/loop.py`:
- import: `from . import analyze, build, config, deploy, playsession, run, scenario, signature`
- `Deps` gains `play_alive: Callable = field(default=playsession.alive)`.
- Add after `_guard_branch`:
```python
def _guard_play(d: Deps) -> int | None:
    info = d.play_alive()
    if info is None:
        return None
    print(f"a play session is running ({info.get('scenario', '?')}, ports {info.get('ports')}); "
          "finish with `python -m debugloop.loop stop-play` before the loop runs a test")
    return HARNESS
```
- At the top of `cmd_next` and `cmd_verify`, right after the `_guard_branch` check: `blocked = _guard_play(d)` / `if blocked is not None: return blocked`. This returns `HARNESS` without touching `harness_errors_in_row` (that counter only moves in `_harness_error`).
- New commands:
```python
def cmd_play(d: Deps, name: str) -> int:
    blocked = _guard_branch(d) or _guard_play(d)
    if blocked is not None:
        return blocked
    scn = scenario.find_scenario(name)
    b = d.build()
    if not b.ok:
        print("BUILD FAILED\n" + b.output[-3000:])
        return HARNESS
    d.deploy()
    try:
        info = d.play_start(scn)
    except run.HarnessError as e:
        print(f"HARNESS ERROR: {e}")
        return HARNESS
    print(f"play session up: {scn.name}  run folder {info['run_dir']}")
    for n, p in info["ports"].items():
        print(f"  {n:8} port {p}")
    print("drive it with the battleborn-play MCP tools or `python -m debugloop.play ...`; "
          "end it with `python -m debugloop.loop stop-play`")
    return OK


def cmd_stop_play(d: Deps) -> int:
    killed = d.play_stop()
    print("stopped: " + ", ".join(killed) if killed else "no play session was running")
    return OK
```
with `Deps` also gaining `play_start: Callable = field(default=playsession.start)` and `play_stop: Callable = field(default=playsession.stop)`. In `main`: `ap.add_argument("command", choices=[..., "play", "stop-play"])`, `ap.add_argument("scenario", nargs="?")`, and dispatch `"play": lambda d: cmd_play(d, a.scenario or "")` (print `usage: loop play <scenario>` and return HARNESS when empty), `"stop-play": cmd_stop_play`. `scenario.ScenarioError` from `find_scenario` is caught in `cmd_play` and printed as `HARNESS ERROR`.

Also fix `_guard_branch(d) or _guard_play(d)`: `_guard_branch` returns `STOPPED` (3, truthy) or `None`, so `or` works.

- [ ] **Step 5: Run the tests** — `python -m pytest debugloop/tests -q`. Expected: all pass. In `test_loop.py`'s `deps()` helper, add `play_alive=lambda: None, play_start=lambda scn: {"run_dir": "r", "ports": {}}, play_stop=lambda: []` to the `loop.Deps(...)` call so existing tests keep their clean state.

- [ ] **Step 6: Commit**

```bash
git add debugloop/playsession.py debugloop/loop.py debugloop/tests/test_playsession.py debugloop/tests/test_loop.py
git commit -m "feat(debugloop): loop play / stop-play sessions for live driving"
```

---

### Task 9: Combat in the ladder scenarios, run warnings, brief text

**Files:**
- Modify: `debugloop/scenarios/s1-dojo-1client.toml`, `s1-dojo-1client-smoke.toml`, `s2-algorithm-2clients.toml`, `s2-algorithm-2clients-smoke.toml`, `s3-meltdown-2clients-bots.toml`, `s3-meltdown-2clients-bots-smoke.toml` (add `"-rbcombat"` to every `role = "client"` args list)
- Modify: `debugloop/run.py` (`RunResult.warnings`, `combat.json`, warnings), `debugloop/loop.py` (`_write_brief`, `cmd_next` print)
- Test: `debugloop/tests/test_run.py`, `debugloop/tests/test_scenario.py`

**Interfaces:**
- Produces: `RunResult.warnings: list[str]` (default empty); `run_dir/combat.json` = `{name: stats dict}` from the last sample of each client; `run.combat_warnings(samples, elapsed_s, min_play_s=600) -> list[str]`.

- [ ] **Step 1: Failing tests**

In `debugloop/tests/test_run.py`:
```python
def test_combat_warnings_for_silent_players():
    from debugloop.outcome import Sample
    quiet = {"autopilot": "playing", "has_pawn": True, "combat": {"enabled": True,
             "stats": {"shots": 0, "damage_taken": 0.0, "kills": 0, "deaths": 0}}}
    busy = {"autopilot": "playing", "has_pawn": True, "combat": {"enabled": True,
            "stats": {"shots": 40, "damage_taken": 900.0, "kills": 1, "deaths": 2}}}
    samples = [Sample(650.0, "c1", quiet, 200, True), Sample(650.0, "c2", busy, 200, True),
               Sample(650.0, "server", {"listening": True}, 200, True)]
    w = run.combat_warnings(samples, elapsed_s=650.0)
    assert w == ["c1: fired 0 shots in 10 min", "c1: took no damage in 10 min"]
    assert run.combat_warnings(samples, elapsed_s=300.0) == []          # too short to judge
    off = dict(quiet, combat={"enabled": False, "stats": quiet["combat"]["stats"]})
    assert run.combat_warnings([Sample(650.0, "c1", off, 200, True)], 650.0) == []   # combat off: no warning


def test_run_writes_combat_json(tmp_path):
    r = run.run_scenario(scn(limit=3), FakeLauncher(), tmp_path, poll_s=0.5)
    j = json.loads((r.run_dir / "combat.json").read_text())
    assert j["c1"]["shots"] == 3
    assert r.warnings == []
```
(the fake game's `/state` must carry `"combat": {"enabled": True, "stats": {...same as its /combat stats...}}` in the client snapshot — add that to `snapshot()` in `fake_game.py` in this task.)

In `debugloop/tests/test_scenario.py`:
```python
def test_ladder_clients_fight():
    from debugloop import scenario
    for s in scenario.load_all():
        for p in s.processes:
            if p.role == "client":
                assert "-rbcombat" in p.args, s.name
```

- [ ] **Step 2: Run** — expected failures (`combat_warnings` missing, scenarios without the flag).

- [ ] **Step 3: Implement**

`run.py`:
```python
def combat_warnings(samples: list, elapsed_s: float, min_play_s: float = 600.0) -> list[str]:
    """Players that never fired or never got hit over a long run: worth a look, not a failure."""
    if elapsed_s < min_play_s:
        return []
    last: dict[str, dict] = {}
    for s in samples:
        if s.state and isinstance(s.state.get("combat"), dict):
            last[s.name] = s.state["combat"]
    mins = int(elapsed_s // 60)
    out = []
    for name, c in last.items():
        if not c.get("enabled"):
            continue
        st = c.get("stats", {})
        if st.get("shots", 0) == 0:
            out.append(f"{name}: fired 0 shots in {mins} min")
        if st.get("damage_taken", 0) == 0:
            out.append(f"{name}: took no damage in {mins} min")
    return out
```
`RunResult` gains `warnings: list[str] = field(default_factory=list)` (import `field`). In `run_scenario` after `sig = signature.make(o, scn)`:
```python
    last_combat = {s.name: s.state["combat"].get("stats", {}) for s in samples
                   if s.state and isinstance(s.state.get("combat"), dict)}
    (run_dir / "combat.json").write_text(json.dumps(last_combat, indent=2))
    result = RunResult(run_id, scn.name, o, sig, run_dir, round(time.time() - t0, 1),
                       milestone(samples), combat_warnings(samples, time.time() - t0))
```
(the dict comprehension keeps the last sample per name because later samples overwrite earlier ones).

`loop.py`: in `cmd_next` after `r = d.run(scn)` succeeds, `for w in r.warnings: print(f"WARNING {w}")`. In `_write_brief`, read `run_dir / "combat.json"` and the run's `result.json` `warnings` list if present; when non-empty add:
```python
    if warnings:
        lines += ["## Combat warnings from the failed run", "", *[f"- {w}" for w in warnings], ""]
```
Scenario files: add `"-rbcombat"` to each client's `args` (six files; s0 untouched). `fake_game.py`: client `snapshot()` gains `"combat": {"enabled": True, "stats": combat()["stats"]}` (define `combat()` above `snapshot()`).

- [ ] **Step 4: Run the tests** — `python -m pytest debugloop/tests -q`. Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add debugloop/run.py debugloop/loop.py debugloop/scenarios debugloop/tests/test_run.py debugloop/tests/test_scenario.py debugloop/tests/fake_game.py
git commit -m "feat(debugloop): combat on in ladder scenarios; combat stats and warnings per run"
```

---

### Task 10: Skill and agent text, notes, live s3 smoke

**Files:**
- Modify: `.claude/skills/bb-autofix/SKILL.md`, `.claude/agents/bb-fixer.md`
- Create/extend: `docs/notes/combat-census-notes.md`

- [ ] **Step 1: `bb-autofix` gets a "Live reproduction" section** (insert before "## 2. Fix"):

```markdown
## 1b. Live reproduction (optional, combat bugs only)

When the brief's signature or triage mentions damage, weapon, skill, death, respawn or fire, and `attempts_on_current` is 0, you may reproduce it by hand before dispatching the fixer:

1. `python -m debugloop.loop play <scenario from the brief>` (background, 15-minute timeout). It prints the player names and ports.
2. Use the `battleborn-play` tools (`bb_list`, `bb_situation`, `bb_order`, `bb_act`, `bb_look`) for at most 10 minutes to steer the players into the situation (for example `bb_order c1 hunt`, then `bb_order c2 follow --target <c1's id>`). Read `bb_log` for `[COMBAT]` and the bug's own `[TAG]` lines. Look at pictures only when the text leaves a doubt.
3. Append what you saw, with the exact log lines, under a `## Live reproduction` heading at the end of `debugloop/state/brief.md`.
4. `python -m debugloop.loop stop-play`. The loop refuses to run while a play session is up.

Skip this when the bug is a startup, join or map-load failure; the loop's own run already shows those.
```

- [ ] **Step 2: `bb-fixer.md` tools list** gains:

```markdown
- Live game tools (read-only for you): `python -m debugloop.play list|situation|combat|log <name>` reads a running play session the controller started. Never run `loop play`, `loop stop-play`, `loop next` or `loop verify`; never start or stop a game.
```

- [ ] **Step 3: Notes file** — make sure `docs/notes/combat-census-notes.md` holds: which pawns the Dojo census reported (Task 1), the calibration numbers and any sign flip (Task 2), which slot is the ultimate and whether `melee` needed `StartFire` or `StartOffHandFire` (Task 3/4). The fixer reads `docs/notes/` by rule, so these facts reach it.

- [ ] **Step 4: Live s3 smoke with combat on**

With no loop run active: `python -m debugloop.run s3-meltdown-2clients-bots-smoke` (background, 20-minute timeout). Expected: `result.json` `outcome.kind == "pass"` (or a match end), `combat.json` with `shots > 0` for both clients, and no new crash signature. If it crashes with a frame inside `reborn.dll` → `Combat::`, fix it in this task (most likely a freed pawn: add a `Gone()` check at the failing read) and re-run once. If it fails for an unrelated reason, record the signature in the notes and hand it to the loop (`loop next` will open it as a bug).

- [ ] **Step 5: Commit**

```bash
git add .claude/skills/bb-autofix/SKILL.md .claude/agents/bb-fixer.md docs/notes/combat-census-notes.md
git commit -m "docs: live reproduction with the play tools; combat census notes"
```

- [ ] **Step 6: Hand back to the loop**

Tell the user: the ladder state is at step 3 with 1 of 2 passes; the next `loop next` runs s3 with combat on. Because scenario args changed, suggest letting the step-3 counter stand (the spec changes what the run tests, not the ladder rule) and resuming `/loop /bb-autofix`.

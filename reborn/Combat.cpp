#include "Combat.hpp"
#include "Engine.hpp"
#include "LaunchOptions.hpp"
#include "PawnUtils.hpp"
#include "Utils.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

namespace Combat {
    namespace {
        using PawnUtils::Gone;

        enum class Mode { Hunt, Hold, Goto, Follow, Retreat, Wander };

        constexpr float kPi = 3.14159265f;
        constexpr float kUnitsPerDeg = 65536.f / 360.f;   // UE3 rotator units
        constexpr float kCensusEvery = 0.25f;
        constexpr int kMaxEnemiesInJson = 12;

        bool g_enabled = false;
        bool g_enabledInit = false;
        float g_sinceCensus = 1e9f;
        std::vector<Enemy> g_enemies;      // nearest first, refreshed every kCensusEvery
        APoplarPlayerController* g_pc = nullptr;
        float g_myLoc[3] = { 0, 0, 0 };
        int g_myYaw = 0, g_myPitch = 0;
        float g_health = 0.f, g_maxHealth = 0.f;
        std::string g_hero;

        // Kept for the whole process (not reset on respawn): the run report's "never fired / never
        // got hit" warnings need the totals.
        struct Stats { int shots = 0, skills = 0, kills = 0, deaths = 0; float damageTaken = 0.f, withTargetS = 0.f, firingS = 0.f; };
        Stats g_stats;
        float g_lastHealth = -1.f;

        Tuning g_tuning;
        Order g_order;
        Mode g_mode = Mode::Hunt;
        float g_modeTime = 0.f;
        Mode g_resumeMode = Mode::Hunt;     // where retreat returns to
        float g_strafeSign = 1.f, g_untilStrafeFlip = 0.f;
        float g_sinceSkill = 0.f;
        float g_burstLeft = 0.f;            // fire_burst: seconds of forced fire remaining
        std::mt19937 g_rng{ 7u };

        // How many rotator units per second one unit of aTurn / aLookUp turns the view. Measured on
        // the first ticks with a pawn: write the probe for one frame, read the change, divide by that
        // frame's dt (PlayerInput scales the axes by dt, so a per-frame figure would drift with the
        // frame rate). The sign comes out of the measurement too (the engine may invert look-up).
        float g_yawPerInput = 0.f, g_pitchPerInput = 0.f;
        int g_calibStep = 0;           // 0 send yaw probe, 1 read yaw + send pitch probe, 2 read pitch
        int g_calibYaw0 = 0, g_calibPitch0 = 0;
        float g_calibProbe = 1.f;
        bool g_calibrated = false;

        uintptr_t g_target = 0;
        float g_targetUnseenS = 0.f;
        bool g_firing = false;
        bool g_aimOn = false;
        Axes g_axes;

        int WrapUnits(int d) { d &= 0xFFFF; return d > 32767 ? d - 65536 : d; }

        std::string HexId(uintptr_t v) {
            char id[32];
            std::snprintf(id, sizeof id, "%llx", (unsigned long long)v);
            return id;
        }

        FVector Vec(float x, float y, float z) { FVector v; v.X = x; v.Y = y; v.Z = z; return v; }
        FVector Eye(APawn* p) { return Vec(p->Location.X, p->Location.Y, p->Location.Z + p->BaseEyeHeight); }
        FVector AimPoint(APawn* p) { return Vec(p->Location.X, p->Location.Y, p->Location.Z + p->BaseEyeHeight * 0.5f); }

        void Exec(const wchar_t* c) { Engine::ExecConsoleCommand(c); }

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
                e.visible = me->FastTrace(aim, eye, Vec(0, 0, 0), false);
                e.health = p->GetHealth();
                e.team = p->GetTeamNum();
                g_enemies.push_back(e);
            }
            std::sort(g_enemies.begin(), g_enemies.end(),
                      [](const Enemy& a, const Enemy& b) { return a.distance < b.distance; });
        }

        void TrackStats(APoplarPlayerController* pc) {
            float h = pc->Pawn->GetHealth();
            if (g_lastHealth >= 0.f && h < g_lastHealth) g_stats.damageTaken += g_lastHealth - h;
            g_lastHealth = h;
            APlayerReplicationInfo* pri = pc->PlayerReplicationInfo;
            if (!Gone(pri)) { g_stats.kills = pri->Kills; g_stats.deaths = pri->Deaths; }
        }

        const Enemy* FindEnemy(uintptr_t id) {
            if (!id) return nullptr;
            for (const Enemy& e : g_enemies) if (e.id == id) return &e;
            return nullptr;
        }

        void SetFiring(bool on) {
            if (on == g_firing) return;
            g_firing = on;
            Exec(on ? L"StartFire" : L"StopFire");
            if (on) g_stats.shots++;
        }

        // Returns true while still calibrating (the axes are the probe; aim must not run).
        bool Calibrate(APoplarPlayerController* pc, float dt) {
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
                g_yawPerInput = d / (g_calibProbe * (std::max)(dt, 0.001f));
                g_calibPitch0 = pc->Rotation.Pitch;
                g_axes = Axes{}; g_axes.forward = 0.f; g_axes.lookUp = g_calibProbe; g_axes.valid = true;
                g_calibStep = 2;
                return true;
            }
            case 2: {
                int d = WrapUnits(pc->Rotation.Pitch - g_calibPitch0);
                g_pitchPerInput = d != 0 ? d / (g_calibProbe * (std::max)(dt, 0.001f)) : g_yawPerInput;
                g_calibrated = true;
                if (std::abs(g_yawPerInput) < 1.f) g_yawPerInput = 6000.f;     // never divide by zero
                if (std::abs(g_pitchPerInput) < 1.f) g_pitchPerInput = g_yawPerInput;
                std::printf("[COMBAT] calibrated: yaw %.0f units/s per input, pitch %.0f (probe %.0f, dt %.4f)\n",
                            g_yawPerInput, g_pitchPerInput, g_calibProbe, dt);
                return false;
            }
            default:
                return false;
            }
        }

        // Choose/keep the target. The order's explicit target wins while it is alive and known.
        void PickTarget(float dt, uintptr_t wanted) {
            if (wanted && FindEnemy(wanted)) { g_target = wanted; g_targetUnseenS = 0.f; return; }
            const Enemy* cur = FindEnemy(g_target);
            if (cur && cur->visible) { g_targetUnseenS = 0.f; return; }
            if (cur) { g_targetUnseenS += dt; if (g_targetUnseenS < g_tuning.targetMemoryS) return; }
            uintptr_t old = g_target;
            g_target = 0; g_targetUnseenS = 0.f;
            float best = 1e12f;
            for (const Enemy& e : g_enemies) {
                if (!e.visible) continue;
                float score = e.distance * (e.kind == "player" ? g_tuning.playerWeight : 1.f);
                if (score < best) { best = score; g_target = e.id; }
            }
            if (g_target && g_target != old)
                std::printf("[COMBAT] target %s (%s, %.0f units)\n", HexId(g_target).c_str(),
                            FindEnemy(g_target)->hero.c_str(), FindEnemy(g_target)->distance);
            else if (!g_target && old)
                std::printf("[COMBAT] target %s dropped\n", HexId(old).c_str());
        }

        float Clamp(float v, float lim) { return (std::max)(-lim, (std::min)(lim, v)); }

        // Turn/look toward the target: one-frame deadbeat step (this frame's dt as the estimate of
        // the next), capped at maxTurnDegPerTick.
        void Aim(const Enemy& e, float dt) {
            float maxUnits = g_tuning.maxTurnDegPerTick * kUnitsPerDeg;
            float myPitchDeg = WrapUnits(g_myPitch) / kUnitsPerDeg;
            float yawErr = Clamp(e.bearingDeg * kUnitsPerDeg, maxUnits);
            float pitchErr = Clamp((e.pitchDeg - myPitchDeg) * kUnitsPerDeg, maxUnits);
            float step = (std::max)(dt, 0.005f);
            g_axes.turn = yawErr / (g_yawPerInput * step);
            g_axes.lookUp = pitchErr / (g_pitchPerInput * step);
            g_aimOn = std::fabs(e.bearingDeg) < g_tuning.aimOnDeg && std::fabs(e.pitchDeg - myPitchDeg) < g_tuning.aimOnDeg;
        }

        bool ParseMode(const std::string& s, Mode& m) {
            if (s == "hunt") m = Mode::Hunt; else if (s == "hold") m = Mode::Hold;
            else if (s == "goto") m = Mode::Goto; else if (s == "follow") m = Mode::Follow;
            else if (s == "retreat") m = Mode::Retreat; else if (s == "wander") m = Mode::Wander;
            else return false;
            return true;
        }

        void SetMode(Mode m) {
            if (m == g_mode) return;
            g_mode = m;
            g_modeTime = 0.f;
            std::printf("[COMBAT] mode %s\n", ModeName());
        }

        // Forward/strafe that walks toward (sign 1) or away from (sign -1) a world point. The view
        // may not face it, so split the direction into the view's forward and right axes.
        void WalkToward(const float* pt, float sign, float stopWithin) {
            float dx = pt[0] - g_myLoc[0], dy = pt[1] - g_myLoc[1];
            float dist = std::sqrt(dx * dx + dy * dy);
            if (dist < stopWithin || dist < 1.f) { g_axes.forward = 0.f; g_axes.strafe = 0.f; return; }
            float yaw = g_myYaw / kUnitsPerDeg * kPi / 180.f;
            float fx = std::cos(yaw), fy = std::sin(yaw);     // view forward
            float rx = -fy, ry = fx;                           // view right (UE3: yaw 90 = +Y = right)
            g_axes.forward = sign * (dx * fx + dy * fy) / dist;
            g_axes.strafe = sign * (dx * rx + dy * ry) / dist;
        }

        // With no target: face the walking goal.
        void TurnToward(const float* pt, float dt) {
            float dx = pt[0] - g_myLoc[0], dy = pt[1] - g_myLoc[1];
            int yawTo = (int)std::lround(std::atan2(dy, dx) * 180.f / kPi * kUnitsPerDeg);
            float err = Clamp((float)WrapUnits(yawTo - g_myYaw), g_tuning.maxTurnDegPerTick * kUnitsPerDeg);
            g_axes.turn = err / (g_yawPerInput * (std::max)(dt, 0.005f));
        }

        void UseSkills(float dt, APoplarPlayerController* pc, const Enemy* t) {
            g_sinceSkill += dt;
            if (!g_order.skills || !t || !t->visible || g_sinceSkill < 2.f) return;
            int nearCount = 0;
            for (const Enemy& e : g_enemies) if (e.distance < 1500.f) nearCount++;
            const EActionSkillSlot slots[3] = { EActionSkillSlot::ASS_SlotOne, EActionSkillSlot::ASS_SlotTwo, EActionSkillSlot::ASS_SlotThree };
            for (int i = 0; i < 3; i++) {
                if (i == 2 && nearCount < 2) continue;                       // ultimate: 2+ enemies nearCount
                if (pc->GetActionSkillSlotCooldownTimeRemaining(slots[i]) > 0.f) continue;
                pc->StartActionSkillBySlot(slots[i]);
                g_stats.skills++;
                g_sinceSkill = 0.f;
                std::printf("[COMBAT] skill slot %d\n", i + 1);
                return;
            }
        }

        std::string Err(const char* word, int status) {
            return std::string("{\"error\":\"") + word + "\",\"status\":" + std::to_string(status) + "}";
        }

        uintptr_t ParseId(const nlohmann::json& v) {
            if (!v.is_string()) return 0;
            return (uintptr_t)std::strtoull(v.get<std::string>().c_str(), nullptr, 16);
        }
    }

    bool Enabled() {
        if (!g_enabledInit) { g_enabled = LaunchOptions::Get().combat; g_enabledInit = true; }
        return g_enabled;
    }

    void SetEnabled(bool on) {
        g_enabledInit = true;
        if (!on) { SetFiring(false); g_axes = Axes{}; g_burstLeft = 0.f; }
        g_enabled = on;
    }

    void Tick(float dt, APoplarPlayerController* pc) {
        g_pc = pc;
        APawn* me = pc->Pawn;
        g_myLoc[0] = me->Location.X; g_myLoc[1] = me->Location.Y; g_myLoc[2] = me->Location.Z;
        g_myYaw = pc->Rotation.Yaw; g_myPitch = pc->Rotation.Pitch;
        g_health = me->GetHealth(); g_maxHealth = me->GetMaxHealth();
        g_hero = PawnUtils::HeroOf(me);
        TrackStats(pc);
        if (Calibrate(pc, dt)) return;
        g_sinceCensus += dt;
        if (g_sinceCensus >= kCensusEvery) { g_sinceCensus = 0.f; Census(pc); }

        g_axes = Axes{}; g_axes.valid = true; g_axes.forward = 0.f;
        g_modeTime += dt;
        PickTarget(dt, g_order.target);
        const Enemy* t = FindEnemy(g_target);
        g_aimOn = false;
        if (t) { Aim(*t, dt); g_stats.withTargetS += dt; }

        // Auto-retreat: low health with an enemy close, in hunt only.
        if (g_mode == Mode::Hunt && g_maxHealth > 0.f && g_health < g_maxHealth * g_tuning.retreatHealthFrac &&
            !g_enemies.empty() && g_enemies[0].distance < g_tuning.retreatNearUnits) {
            g_resumeMode = Mode::Hunt;
            SetMode(Mode::Retreat);
        }

        switch (g_mode) {
        case Mode::Hunt:
            if (t) {
                WalkToward(t->loc, 1.f, g_tuning.engageRange);
                g_untilStrafeFlip -= dt;
                if (g_untilStrafeFlip <= 0.f) {
                    g_strafeSign = -g_strafeSign;
                    g_untilStrafeFlip = std::uniform_real_distribution<float>(1.5f, 3.f)(g_rng);
                }
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
            if (!t) TurnToward(g_order.point, dt);
            if (g_axes.forward == 0.f && g_axes.strafe == 0.f) SetMode(Mode::Hold);
            break;
        case Mode::Follow: {
            const Enemy* f = FindEnemy(g_order.target);
            if (!f) { SetMode(Mode::Hold); break; }
            if (f->distance > 1200.f) WalkToward(f->loc, 1.f, 1200.f);
            else if (f->distance < 600.f) WalkToward(f->loc, -1.f, 0.f);
            if (!t) TurnToward(f->loc, dt);
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
        if (g_burstLeft > 0.f) { g_burstLeft -= dt; fire = true; }
        SetFiring(fire);
        if (fire) g_stats.firingS += dt;
        UseSkills(dt, pc, t);
    }

    void OnNoPawn() {
        g_enemies.clear();
        g_lastHealth = -1.f;
        g_health = 0.f;
        g_target = 0;
        g_burstLeft = 0.f;
        g_axes = Axes{};
        SetFiring(false);
    }

    Axes CurrentAxes() { return g_axes; }
    Tuning& GetTuning() { return g_tuning; }
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
        case Mode::Hunt: return "hunt";
        case Mode::Hold: return "hold";
        case Mode::Goto: return "goto";
        case Mode::Follow: return "follow";
        case Mode::Retreat: return "retreat";
        default: return "wander";
        }
    }

    nlohmann::json StatsJson() {
        return { {"shots", g_stats.shots}, {"skills_used", g_stats.skills}, {"kills", g_stats.kills},
                 {"deaths", g_stats.deaths}, {"damage_taken", g_stats.damageTaken},
                 {"time_with_target_s", g_stats.withTargetS}, {"time_firing_s", g_stats.firingS} };
    }

    std::string SnapshotJson() {
        nlohmann::json j;
        j["enabled"] = Enabled();
        j["mode"] = ModeName();
        j["calibrated"] = g_calibrated;
        j["aim_on"] = g_aimOn;
        j["firing"] = g_firing;
        bool alive = g_pc && !Gone(g_pc) && !Gone(g_pc->Pawn);
        j["me"] = { {"hero", g_hero}, {"location", {g_myLoc[0], g_myLoc[1], g_myLoc[2]}},
                    {"yaw", g_myYaw / kUnitsPerDeg}, {"pitch", WrapUnits(g_myPitch) / kUnitsPerDeg},
                    {"health", g_health}, {"max_health", g_maxHealth}, {"dead", !alive} };
        nlohmann::json skills = nlohmann::json::array();
        if (alive) {
            for (int i = 0; i < 3; i++) {
                float cd = g_pc->GetActionSkillSlotCooldownTimeRemaining((EActionSkillSlot)i);
                skills.push_back({ {"slot", i + 1}, {"ready", cd <= 0.f}, {"cooldown_s", cd} });
            }
        }
        j["me"]["skills"] = skills;
        j["target"] = g_target ? nlohmann::json(HexId(g_target)) : nlohmann::json(nullptr);
        j["order"] = { {"mode", g_order.mode}, {"fire", g_order.fire}, {"skills", g_order.skills},
                       {"target", g_order.target ? nlohmann::json(HexId(g_order.target)) : nlohmann::json(nullptr)},
                       {"point", g_order.hasPoint ? nlohmann::json({ g_order.point[0], g_order.point[1], g_order.point[2] })
                                                  : nlohmann::json(nullptr)} };
        nlohmann::json arr = nlohmann::json::array();
        int n = 0;
        for (const Enemy& e : g_enemies) {
            if (n++ >= kMaxEnemiesInJson) break;
            arr.push_back({ {"id", HexId(e.id)}, {"hero", e.hero}, {"kind", e.kind}, {"distance", e.distance},
                            {"bearing", e.bearingDeg}, {"pitch", e.pitchDeg}, {"visible", e.visible},
                            {"health", e.health}, {"team", e.team},
                            {"location", {e.loc[0], e.loc[1], e.loc[2]}} });
        }
        j["enemies"] = arr;
        j["n_enemies"] = g_enemies.size();
        j["stats"] = StatsJson();
        return j.dump();
    }

    std::string ApplyOrderJson(const std::string& body) {
        nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
        if (j.is_discarded() || !j.is_object()) return Err("bad_json", 400);
        try {
            Order o = g_order;
            if (j.contains("mode")) { if (!j["mode"].is_string()) return Err("unknown_mode", 400); o.mode = j["mode"]; }
            if (j.contains("target")) o.target = j["target"].is_null() ? 0 : ParseId(j["target"]);
            if (j.contains("point")) {
                if (j["point"].is_null()) o.hasPoint = false;
                else if (j["point"].is_array() && j["point"].size() == 3) {
                    o.hasPoint = true;
                    for (int i = 0; i < 3; i++) o.point[i] = j["point"][i].get<float>();
                }
                else return Err("bad_point", 400);
            }
            if (j.contains("fire")) o.fire = j["fire"].get<bool>();
            if (j.contains("skills")) o.skills = j["skills"].get<bool>();
            Tuning t = g_tuning;
            if (j.contains("tuning") && j["tuning"].is_object()) {
                for (auto& [k, v] : j["tuning"].items()) {
                    float f = v.get<float>();
                    if (k == "fireRange") t.fireRange = f; else if (k == "engageRange") t.engageRange = f;
                    else if (k == "aimOnDeg") t.aimOnDeg = f; else if (k == "maxTurnDegPerTick") t.maxTurnDegPerTick = f;
                    else if (k == "retreatHealthFrac") t.retreatHealthFrac = f; else if (k == "retreatNearUnits") t.retreatNearUnits = f;
                    else if (k == "playerWeight") t.playerWeight = f; else if (k == "targetMemoryS") t.targetMemoryS = f;
                }
            }
            bool enabled = j.contains("enabled") ? j["enabled"].get<bool>() : Enabled();
            std::string err = SetOrder(o);
            if (!err.empty()) return Err(err.c_str(), 400);
            g_tuning = t;
            SetEnabled(enabled);
        }
        catch (const nlohmann::json::exception&) {
            return Err("bad_json", 400);   // a field of the wrong type
        }
        return SnapshotJson();
    }

    std::string ActJson(const std::string& body) {
        nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
        if (j.is_discarded() || !j.is_object() || !j.contains("action") || !j["action"].is_string()) return Err("bad_json", 400);
        std::string a = j["action"];
        float dur = j.contains("duration_s") && j["duration_s"].is_number() ? j["duration_s"].get<float>() : 0.5f;
        APoplarPlayerController* pc = SDKUtils::GetLocalPlayerController();
        if (!pc || Gone(pc->Pawn)) return Err("no_pawn", 409);
        if (a == "jump") Exec(L"Jump");
        else if (a == "fire_start") { g_order.fire = true; SetFiring(true); }
        else if (a == "fire_stop") { SetFiring(false); g_order.fire = false; }
        else if (a == "fire_burst") {
            if (!Enabled()) return Err("combat_off", 409);   // the burst timer runs in the combat tick
            g_burstLeft = dur;
            SetFiring(true);
        }
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
}

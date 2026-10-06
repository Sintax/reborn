#include "Combat.hpp"
#include "Engine.hpp"
#include "LaunchOptions.hpp"
#include "PawnUtils.hpp"
#include "Utils.hpp"
#include <algorithm>
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
        float g_myLoc[3] = { 0, 0, 0 };
        int g_myYaw = 0, g_myPitch = 0;
        float g_health = 0.f, g_maxHealth = 0.f;
        std::string g_hero;

        struct Stats { int shots = 0, skills = 0, kills = 0, deaths = 0; float damageTaken = 0.f, withTargetS = 0.f, firingS = 0.f; };
        Stats g_stats;
        float g_lastHealth = -1.f;

        int WrapUnits(int d) { d &= 0xFFFF; return d > 32767 ? d - 65536 : d; }

        std::string HexId(uintptr_t v) {
            char id[32];
            std::snprintf(id, sizeof id, "%llx", (unsigned long long)v);
            return id;
        }

        FVector Vec(float x, float y, float z) { FVector v; v.X = x; v.Y = y; v.Z = z; return v; }
        FVector Eye(APawn* p) { return Vec(p->Location.X, p->Location.Y, p->Location.Z + p->BaseEyeHeight); }
        FVector AimPoint(APawn* p) { return Vec(p->Location.X, p->Location.Y, p->Location.Z + p->BaseEyeHeight * 0.5f); }

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
        j["mode"] = "wander";
        bool alive = g_pc && !Gone(g_pc) && !Gone(g_pc->Pawn);
        j["me"] = { {"hero", g_hero}, {"location", {g_myLoc[0], g_myLoc[1], g_myLoc[2]}},
                    {"yaw", g_myYaw / kUnitsPerDeg}, {"pitch", WrapUnits(g_myPitch) / kUnitsPerDeg},
                    {"health", g_health}, {"max_health", g_maxHealth}, {"dead", !alive} };
        j["target"] = nullptr;
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
}

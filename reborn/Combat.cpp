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
        constexpr float kSeenWithinS = 0.3f;    // drawn this recently counts as visible

        bool g_enabled = false;
        bool g_enabledInit = false;
        float g_sinceCensus = 1e9f;
        std::vector<Enemy> g_enemies;      // nearest first, refreshed every kCensusEvery
        int g_censusPawns = 0, g_censusLive = 0;   // last census: pawns walked, live ones (diagnostics)
        uintptr_t g_friend = 0;                    // nearest friendly bot hero (0 = none)
        float g_friendDist = 1e12f;
        float g_friendLoc[3] = { 0, 0, 0 };
        int g_friendKinds[3] = { 0, 0, 0 };        // last census: non-enemies by kind (bot, player, other)
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
        // Meltdown smoke 20261006-071629: enemies 25-30k units away count as visible on an open map;
        // chasing them in a straight line left both players facing a wall near base, using their
        // skills at that range. Targets, chases and skills now stay local.
        constexpr float kChaseRange = 6000.f, kSkillRange = 2000.f;
        constexpr float kFollowFriendUnits = 500.f;   // stay this close to the friendly bot we follow
        constexpr float kStuckCheckS = 4.f, kStuckMoveUnits = 150.f, kWanderAfterStuckS = 6.f;
        float g_wanderFor = 0.f, g_chaseCheckS = 0.f;
        float g_chaseFrom[2] = { 0, 0 };
        // Reach: how far this hero's attacks actually hurt. Starts unlimited; shrinks when firing at a
        // visible target does no damage for kNoDamageS. Kept for the process (the hero does not change).
        constexpr float kNoDamageS = 2.5f, kMinReach = 200.f;
        float g_reach = 1e9f;
        float g_noDamageS = 0.f, g_reachTargetHealth = -1.f;
        uintptr_t g_reachTarget = 0;
        std::mt19937 g_rng{ 7u };

        // The view turns through aTurn/aLookUp, like a player's stick. Writing Rotation (directly or
        // with ClientSetRotation) does not stick: live (20261006-054516) the controller's yaw was back
        // at the same stored value every tick. The input's speed is not a fixed number either (two
        // runs measured 15285 and 22187 rotator units per second at input 1; look-up is inverted,
        // about -30800), so the rate is learned while turning: each tick compares the view's change
        // with the input written the tick before. Steering is proportional to the live error, so a
        // rate off by 2x only makes it a little faster or slower, never unstable.
        constexpr float kAimGainPerS = 8.f;           // close 1/8 of the error per 1/8 s
        constexpr int kMaxPitchUnits = 16000;         // ~88 degrees up or down
        float g_yawRate = 18000.f, g_pitchRate = -30000.f;   // learned units/s at input 1
        float g_prevTurnIn = 0.f, g_prevLookIn = 0.f; // input written last tick
        int g_prevYaw = 0, g_prevPitch = 0;
        bool g_havePrev = false;

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

        // Why the last census left out the other pawns (diagnostics, in /combat "census.dropped").
        enum Drop { DropGone, DropTornOff, DropNotInLevel, DropDead, DropNotEnemy, DropCount };
        int g_dropped[DropCount] = { 0 };
        nlohmann::json g_othersSample = nlohmann::json::array();   // first few other pawns, as the census saw them

        // A minion's health as its ResourcePoolManager replicates it: HealthPool names the manager
        // and the pool's slot there (ReplicatedValueStates is the replicated copy of each pool's value).
        // -1 when the manager or the slot is not known on this machine.
        float ReplicatedPoolHealth(APawn* p) {
            AResourcePoolManager* m = p->HealthPool.PoolManager;
            uint8_t i = p->HealthPool.PoolIndexInManager;
            if (Gone(m) || i >= 16) return -1.f;
            if (m->ReplicatedPoolIdentities[i].PoolGUID != p->HealthPool.PoolGUID) return -1.f;
            return m->ReplicatedValueStates[i].CurrentValue;
        }

        void Census(APoplarPlayerController* pc) {
            g_enemies.clear();
            g_censusPawns = g_censusLive = 0;
            for (int& d : g_dropped) d = 0;
            g_othersSample = nlohmann::json::array();
            g_friend = 0;
            g_friendDist = 1e12f;
            g_friendKinds[0] = g_friendKinds[1] = g_friendKinds[2] = 0;
            APawn* me = pc->Pawn;
            if (Gone(me) || !me->IsA(APoplarPawn::StaticClass())) return;
            APoplarPawn* mePop = reinterpret_cast<APoplarPawn*>(me);
            AWorldInfo* wi = pc->WorldInfo;
            if (Gone(wi)) return;
            const bool client = wi->NetMode == ENetMode::NM_Client;
            const uint8_t myTeam = me->GetTeamNum();
            FVector eye = Eye(me);
            int viewYaw = pc->Rotation.Yaw;
            int guard = 0;
            for (APawn* p = wi->PawnList; p && guard < 2000; p = p->NextPawn, guard++) {
                g_censusPawns++;
                if (p == me) continue;
                Drop drop = DropCount;
                if (Gone(p) || p->bDeleteMe) drop = DropGone;
                else if (p->bTearOff) drop = DropTornOff;
                else if (!p->Outer || !p->Outer->IsA(ULevel::StaticClass())) drop = DropNotInLevel;

                // Health. A player's or bot hero's pool replicates through its PRI. A minion has no
                // PRI, and on a networked client GetHealth can read 0 for a live one (live run
                // 20261007-071315: the client's PawnList had 4 Dojo minions, the census counted 0
                // live), so fall back to the value its pool manager replicates; if that is unknown
                // too, it counts as alive (health -1 = unknown) unless it has gone ragdoll.
                float health = 0.f, nativeHealth = 0.f, poolHealth = -1.f;
                const bool minion = Gone(p->PlayerReplicationInfo);
                if (drop == DropCount) {
                    nativeHealth = health = p->GetHealth();
                    bool alive = health > 0.f;
                    if (!alive && client && minion) {
                        poolHealth = ReplicatedPoolHealth(p);
                        if (poolHealth > 0.f) { health = poolHealth; alive = true; }
                        else if (poolHealth < 0.f && p->Physics != EPhysics::PHYS_RigidBody) { health = -1.f; alive = true; }
                    }
                    if (!alive) drop = DropDead;
                }

                // Enemy. On a client the native IFF can lack what it needs for a minion (its AI
                // controller and allegiance live on the server), so a minion on another known team
                // counts as an enemy too.
                bool nativeEnemy = false, enemy = false;
                uint8_t team = 255;
                if (drop == DropCount) {
                    nativeEnemy = enemy = mePop->IsEnemy(p);
                    team = p->GetTeamNum();
                    if (!enemy && client && minion && team != 255 && myTeam != 255 && team != myTeam) enemy = true;
                }

                if (g_othersSample.size() < 6) {
                    g_othersSample.push_back({ {"name", Gone(p) ? std::string("gone") : PawnUtils::HeroOf(p)},
                        {"kind", Gone(p) ? std::string("?") : PawnUtils::KindOf(p)},
                        {"dropped", drop == DropGone ? "gone" : drop == DropTornOff ? "torn_off" : drop == DropNotInLevel ? "not_in_level"
                                  : drop == DropDead ? "dead" : enemy ? "no (enemy)" : "no (not enemy)"},
                        {"health", health}, {"get_health", nativeHealth}, {"pool_health", poolHealth},
                        {"team", (int)team}, {"my_team", (int)myTeam}, {"is_enemy_native", nativeEnemy},
                        {"physics", Gone(p) ? -1 : (int)p->Physics} });
                }
                if (drop != DropCount) { g_dropped[drop]++; continue; }
                g_censusLive++;
                if (!enemy) {
                    g_dropped[DropNotEnemy]++;
                    // A friendly bot hero: the game's own AI walks it along the lanes to the fight.
                    std::string kind = PawnUtils::KindOf(p);
                    g_friendKinds[kind == "bot" ? 0 : kind == "player" ? 1 : 2]++;
                    if (kind == "bot") {
                        float fx = p->Location.X - me->Location.X, fy = p->Location.Y - me->Location.Y;
                        float d = std::sqrt(fx * fx + fy * fy);
                        if (d < g_friendDist) {
                            g_friendDist = d;
                            g_friend = reinterpret_cast<uintptr_t>(p);
                            g_friendLoc[0] = p->Location.X; g_friendLoc[1] = p->Location.Y; g_friendLoc[2] = p->Location.Z;
                        }
                    }
                    continue;
                }
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
                // "Can I see it" = the renderer drew it in the last moment. FastTrace through
                // ProcessEvent said false for minions in plain view (live run 20261006-042128).
                e.visible = wi->TimeSeconds - p->LastRenderTime < kSeenWithinS;
                e.health = health;   // -1 = unknown (a client-side minion, see above)
                e.team = team;
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

        // Through the controller's own StartFire/StopFire: the console's "StartFire" did nothing
        // (live 20261006-055622: Oscar Mike's magazine stayed at 30 through 13 "shots"). Battleborn's
        // buttons go through its own input package (GD_Input_Poplar), not console exec bindings.
        void SetFiring(bool on) {
            if (on == g_firing) return;
            g_firing = on;
            APoplarPlayerController* pc = SDKUtils::GetLocalPlayerController();
            if (pc && !Gone(pc->Pawn)) {
                if (on) pc->StartFire(0); else pc->StopFire(0);
            }
            if (on) g_stats.shots++;
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
                if (!e.visible || e.distance > kChaseRange) continue;
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

        // A live pawn by id, re-found in the world's pawn list so a pawn freed since the census is
        // never read. Null when it is gone.
        APawn* LivePawn(uintptr_t id) {
            if (!id || !g_pc || Gone(g_pc->WorldInfo)) return nullptr;
            int guard = 0;
            for (APawn* p = g_pc->WorldInfo->PawnList; p && guard < 2000; p = p->NextPawn, guard++)
                if (reinterpret_cast<uintptr_t>(p) == id) return PawnUtils::LivePawnInWorld(p) ? p : nullptr;
            return nullptr;
        }

        // Learn the input's turn rate from what last tick's input did (call once per tick, before
        // writing new input).
        void LearnRates(APoplarPlayerController* pc, float dt) {
            int yaw = pc->Rotation.Yaw, pitch = WrapUnits(pc->Rotation.Pitch);
            if (g_havePrev && dt > 0.f) {
                float dYaw = (float)WrapUnits(yaw - g_prevYaw), dPitch = (float)(pitch - g_prevPitch);
                if (std::fabs(g_prevTurnIn) > 0.05f) {
                    float r = dYaw / (g_prevTurnIn * dt);
                    if (r > 3000.f && r < 120000.f) g_yawRate += 0.1f * (r - g_yawRate);
                }
                if (std::fabs(g_prevLookIn) > 0.05f && std::abs(pitch) < kMaxPitchUnits - 500) {
                    float r = dPitch / (g_prevLookIn * dt);
                    if (std::fabs(r) > 3000.f && std::fabs(r) < 120000.f && (r > 0) == (g_pitchRate > 0))
                        g_pitchRate += 0.1f * (r - g_pitchRate);
                }
            }
            g_prevYaw = yaw; g_prevPitch = pitch; g_havePrev = true;
            g_prevTurnIn = g_prevLookIn = 0.f;
        }

        // Steer the view toward a world point. Returns the live yaw and pitch error in degrees.
        void TurnViewTo(const FVector& from, const FVector& to, float dt, float& yawErrDeg, float& pitchErrDeg) {
            float dx = to.X - from.X, dy = to.Y - from.Y, dz = to.Z - from.Z;
            float flat = std::sqrt(dx * dx + dy * dy);
            int yawTo = (int)std::lround(std::atan2(dy, dx) * 180.f / kPi * kUnitsPerDeg);
            int pitchTo = (int)std::lround(std::atan2(dz, flat) * 180.f / kPi * kUnitsPerDeg);
            pitchTo = (std::max)(-kMaxPitchUnits, (std::min)(kMaxPitchUnits, pitchTo));
            float yawErr = (float)WrapUnits(yawTo - g_pc->Rotation.Yaw);
            float pitchErr = (float)(pitchTo - WrapUnits(g_pc->Rotation.Pitch));
            float maxRate = g_tuning.maxTurnDegPerS * kUnitsPerDeg;
            float wantYawRate = Clamp(kAimGainPerS * yawErr, maxRate);
            float wantPitchRate = Clamp(kAimGainPerS * pitchErr, maxRate);
            g_axes.turn = g_prevTurnIn = wantYawRate / g_yawRate;
            g_axes.lookUp = g_prevLookIn = wantPitchRate / g_pitchRate;
            yawErrDeg = yawErr / kUnitsPerDeg;
            pitchErrDeg = pitchErr / kUnitsPerDeg;
        }

        // Aim at the target's live position (the census copy is up to kCensusEvery old).
        void Aim(const Enemy& e, float dt) {
            APawn* me = g_pc->Pawn;
            APawn* tp = LivePawn(e.id);
            FVector to = tp ? AimPoint(tp) : Vec(e.loc[0], e.loc[1], e.loc[2]);
            float yawLeft = 0.f, pitchLeft = 0.f;
            TurnViewTo(Eye(me), to, dt, yawLeft, pitchLeft);
            g_aimOn = std::fabs(yawLeft) < g_tuning.aimOnDeg && std::fabs(pitchLeft) < g_tuning.aimOnDeg;
        }

        void LearnReach(float dt, const Enemy* t, bool firing) {
            // Health -1 = unknown (a client-side minion): no drop to learn from, keep the reach as is.
            if (!firing || !t || t->health < 0.f) { g_noDamageS = 0.f; return; }
            if (t->id != g_reachTarget || t->health < g_reachTargetHealth) {   // new target, or it got hurt
                // A hit near or past the learned reach: it was too short (shots at an enemy behind
                // cover can shrink it), so grow it back.
                if (t->id == g_reachTarget && g_reach < 1e8f && t->distance > g_reach * 0.8f) {
                    g_reach = t->distance * 1.5f;
                    std::printf("[COMBAT] hit at %.0f units; reach now %.0f\n", t->distance, g_reach);
                }
                g_reachTarget = t->id;
                g_reachTargetHealth = t->health;
                g_noDamageS = 0.f;
                return;
            }
            g_noDamageS += dt;
            if (g_noDamageS < kNoDamageS) return;
            g_noDamageS = 0.f;
            float r = (std::max)(kMinReach, t->distance * 0.6f);
            if (r < g_reach) {
                g_reach = r;
                std::printf("[COMBAT] no damage after %.1f s at %.0f units; reach now %.0f\n", kNoDamageS, t->distance, g_reach);
            }
        }

        // Hunt: when the brain has been trying to walk for kStuckCheckS and moved less than
        // kStuckMoveUnits, hand movement to the autopilot's wander for kWanderAfterStuckS.
        // inFight: strafing back and forth in range moves little on purpose; that is not stuck.
        void CheckChaseProgress(float dt, bool inFight) {
            bool walking = !inFight && g_axes.valid && (g_axes.forward != 0.f || g_axes.strafe != 0.f);
            if (!walking) { g_chaseCheckS = 0.f; g_chaseFrom[0] = g_myLoc[0]; g_chaseFrom[1] = g_myLoc[1]; return; }
            g_chaseCheckS += dt;
            if (g_chaseCheckS < kStuckCheckS) return;
            float dx = g_myLoc[0] - g_chaseFrom[0], dy = g_myLoc[1] - g_chaseFrom[1];
            if (std::sqrt(dx * dx + dy * dy) < kStuckMoveUnits) {
                g_wanderFor = kWanderAfterStuckS;
                std::printf("[COMBAT] chase stuck; wandering for %.0f s\n", kWanderAfterStuckS);
            }
            g_chaseCheckS = 0.f;
            g_chaseFrom[0] = g_myLoc[0]; g_chaseFrom[1] = g_myLoc[1];
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

        // With no target: face the walking goal, eyes level.
        void TurnToward(const float* pt, float dt) {
            APawn* me = g_pc->Pawn;
            FVector eye = Eye(me);
            float yawLeft = 0.f, pitchLeft = 0.f;
            TurnViewTo(eye, Vec(pt[0], pt[1], eye.Z), dt, yawLeft, pitchLeft);
        }

        void UseSkills(float dt, APoplarPlayerController* pc, const Enemy* t) {
            g_sinceSkill += dt;
            if (!g_order.skills || !t || !t->visible || t->distance > kSkillRange || g_sinceSkill < 2.f) return;
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
        LearnRates(pc, dt);
        g_sinceCensus += dt;
        if (g_sinceCensus >= kCensusEvery) { g_sinceCensus = 0.f; Census(pc); }

        // The learned reach caps both ranges, so a melee hero (Rath: a sword) closes in instead of
        // swinging from 30 m away (live 20261006-054808: 20 s of firing, no damage). The weapon's own
        // WeaponRange is no help: it read 16384 for Rath's sword.
        float fireRange = (std::min)(g_tuning.fireRange, g_reach);
        float engageRange = (std::min)(g_tuning.engageRange, g_reach * 0.7f);

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
            if (g_wanderFor > 0.f) {
                // A straight-line chase got stuck: let the autopilot wander (and its unstick turn)
                // move us for a while, still shooting at anything in sight.
                g_wanderFor -= dt;
                g_axes.valid = false;
            } else if (t) {
                WalkToward(t->loc, 1.f, engageRange);
                g_untilStrafeFlip -= dt;
                if (g_untilStrafeFlip <= 0.f) {
                    g_strafeSign = -g_strafeSign;
                    g_untilStrafeFlip = std::uniform_real_distribution<float>(1.5f, 3.f)(g_rng);
                }
                if (t->distance <= engageRange) g_axes.strafe = g_strafeSign * 0.7f;
            } else if (!g_enemies.empty() && g_enemies[0].distance < kChaseRange) {
                // Nothing in sight but the census knows an enemy nearby: head for it (straight line).
                WalkToward(g_enemies[0].loc, 1.f, 0.f);
                TurnToward(g_enemies[0].loc, dt);
            } else if (g_friend) {
                // The fight is far away: tag along with the nearest friendly bot hero, whose AI
                // paths along the lanes (live Meltdown: straight lines ended against walls).
                if (g_friendDist > kFollowFriendUnits) {
                    WalkToward(g_friendLoc, 1.f, kFollowFriendUnits);
                    TurnToward(g_friendLoc, dt);
                }
            } else {
                g_axes.valid = false;   // nothing near and no bot to follow: the autopilot wanders
            }
            CheckChaseProgress(dt, t && t->distance <= engageRange);
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

        bool fire = g_order.fire && t && g_aimOn && t->visible && t->distance < fireRange;
        LearnReach(dt, t, fire);
        if (g_burstLeft > 0.f) { g_burstLeft -= dt; fire = true; }
        SetFiring(fire);
        if (fire) g_stats.firingS += dt;
        UseSkills(dt, pc, t);
        // The autopilot writes our turn input only when the axes are valid; otherwise its own wander
        // turn moves the view, and learning from it would corrupt the rates.
        if (!g_axes.valid) g_prevTurnIn = g_prevLookIn = 0.f;
    }

    void OnNoPawn() {
        g_pc = nullptr;   // may be freed before the next pawn arrives
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
        j["aim_on"] = g_aimOn;
        j["turn_rates"] = { g_yawRate, g_pitchRate };
        j["reach"] = g_reach < 1e8f ? nlohmann::json(g_reach) : nlohmann::json(nullptr);
        j["firing"] = g_firing;
        // A fresh lookup, never g_pc: the controller from the last combat tick may be freed by now
        // (match over, back in the menu).
        APoplarPlayerController* livePc = SDKUtils::GetLocalPlayerController();
        bool alive = livePc && !Gone(livePc->Pawn);
        j["me"] = { {"hero", g_hero}, {"location", {g_myLoc[0], g_myLoc[1], g_myLoc[2]}},
                    {"yaw", g_myYaw / kUnitsPerDeg}, {"pitch", WrapUnits(g_myPitch) / kUnitsPerDeg},
                    {"health", g_health}, {"max_health", g_maxHealth}, {"dead", !alive} };
        nlohmann::json skills = nlohmann::json::array();
        if (alive) {
            for (int i = 0; i < 3; i++) {
                float cd = livePc->GetActionSkillSlotCooldownTimeRemaining((EActionSkillSlot)i);
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
        j["census"] = { {"pawns", g_censusPawns}, {"live_others", g_censusLive},
                        {"following", g_friend ? nlohmann::json(HexId(g_friend)) : nlohmann::json(nullptr)},
                        {"friend_distance", g_friend ? nlohmann::json(g_friendDist) : nlohmann::json(nullptr)},
                        {"friendly_bots", g_friendKinds[0]}, {"friendly_players", g_friendKinds[1]},
                        {"friendly_other", g_friendKinds[2]},
                        {"dropped", { {"gone", g_dropped[DropGone]}, {"torn_off", g_dropped[DropTornOff]},
                                      {"not_in_level", g_dropped[DropNotInLevel]}, {"dead", g_dropped[DropDead]},
                                      {"not_enemy", g_dropped[DropNotEnemy]} }},
                        {"others", g_othersSample} };
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
                    else if (k == "aimOnDeg") t.aimOnDeg = f; else if (k == "maxTurnDegPerS") t.maxTurnDegPerS = f;
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
        else if (a == "altfire") { pc->StartAltFire(0); pc->StopAltFire(0); }
        else if (a == "melee") { pc->StartOffHandFire(0); pc->StopOffHandFire(0); }
        else if (a == "skill1") pc->StartActionSkillBySlot(EActionSkillSlot::ASS_SlotOne);
        else if (a == "skill2") pc->StartActionSkillBySlot(EActionSkillSlot::ASS_SlotTwo);
        else if (a == "ultimate") pc->StartActionSkillBySlot(EActionSkillSlot::ASS_SlotThree);
        // Aimed skills (placed bombs, traps, rifts) wait in an aiming preview after the press;
        // confirm casts the one being aimed, like releasing the key or clicking.
        else if (a == "confirm") pc->ConfirmActionSkill();
        else if (a == "cancel_skill") pc->CancelActionSkill();
        else if (a == "use") pc->Use();
        else if (a == "sprint_start") pc->BeginSprint();
        else if (a == "sprint_stop") pc->EndSprint();
        else return Err("unknown_action", 400);
        return "{\"ok\":true,\"action\":\"" + a + "\"}";
    }
}

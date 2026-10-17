#include "CharSelect.hpp"
#include "Engine.hpp"
#include "Globals.hpp"
#include "Hooks.hpp"
#include "Networking.hpp"
#include "ServerSettings.hpp"
#include "Utils.hpp"
#include <cstdio>
#include <string>
#include <unordered_set>

namespace CharSelect {
    namespace {
        const uint64_t kPendingKill = 0x2000000000000000ull; // RF_PendingKill
        const float kWaitingForPlayersSeconds = 120.f;
        const float kRandomHeroPickSeconds = 5.f;

        bool g_rulesApplied = false; // ApplyServerRules ran; the rules dump waits for it

        bool IsTemplate(UObject* o) { return o->GetFullName().find("Default__") != std::string::npos; }

        // The game's shipped select rules: every style with its phase order, every phase, and every
        // game type's select settings.
        void DumpRules() {
            for (UPoplarCharacterSelectStyleDefinition* s : SDKUtils::GetAllOfClass<UPoplarCharacterSelectStyleDefinition>()) {
                if (!s || IsTemplate(s)) continue;
                std::string phases;
                for (int i = 0; i < s->CharacterSelectPhaseDefs.ArrayCount; i++) {
                    UPoplarCharacterSelectPhaseDefinition* p = s->CharacterSelectPhaseDefs[i];
                    phases += (phases.empty() ? "" : " > ") + (p ? p->GetName() : std::string("null"));
                }
                printf("[CHARSEL] style %s \"%s\" unique %d botpick %.0fs blind %u pve %u 5v5 %u 3v3 %u: %s\n", s->GetName().c_str(),
                    s->StyleName.ToString().c_str(), (int)s->UniquenessLevel, s->SimulatedBotSelectionTime, s->bBlindSelection,
                    s->bSupports_PvE, s->bSupports_PvP_5v5, s->bSupports_PvP_3v3, phases.c_str());
            }
            for (UPoplarCharacterSelectPhaseDefinition* p : SDKUtils::GetAllOfClass<UPoplarCharacterSelectPhaseDefinition>()) {
                if (!p || IsTemplate(p)) continue;
                std::string setup;
                if (p->IsA(UPoplarCharacterSelectPhaseDefinition_Setup::StaticClass())) {
                    auto* su = reinterpret_cast<UPoplarCharacterSelectPhaseDefinition_Setup*>(p);
                    char b[160];
                    snprintf(b, sizeof b, " setup: nextWhenAllConnected %u nextWhenAllEntitled %u nextWhenStandaloneEntitled %u abandonOnTimeout %u",
                        su->bMoveToNextPhaseWhenAllConnected, su->bMoveToNextPhaseWhenAllPlayersHaveEntitlements,
                        su->bMoveToNextPhaseWhenReceivedStandaloneEntitlements, su->bAbandonMatchIfTimeRunsOut);
                    setup = b;
                }
                printf("[CHARSEL] phase %s type %d who %d duration %.0fs picks/player %d randomizeRest %u botsAtStart %u botOnlyIfNoHuman %u \"%s\"%s\n",
                    p->GetName().c_str(), (int)p->PhaseType, (int)p->ActiveSelectingType, p->DurationOfPhase, p->SelectionsAllowedPerActivePlayer,
                    p->bRandomizeRemainingSelections, p->bSpawnBotsAtBeginningOfPhase, p->bAllowBotToSelectOnlyIfNoHumanPlayerOnTeam,
                    p->PhaseName.ToString().c_str(), setup.c_str());
            }
            for (UPoplarGameInfoDefinition* d : SDKUtils::GetAllOfClass<UPoplarGameInfoDefinition>()) {
                if (!d || IsTemplate(d)) continue;
                printf("[CHARSEL] gid %s pve %u ops %u uniquePerTeam %u setupTimeout %.0fs selectLevel %s\n", d->GetName().c_str(), d->bPvE, d->bOperations,
                    d->RequireUniqueCharactersPerTeam, d->SetupTimeoutTime, d->CharacterSelectLevelName.ToString().c_str());
            }
        }
    }

    void ApplyServerRules(bool randomHero) {
        UPoplarCharacterSelectPhaseDefinition* waitAbandon = nullptr;
        UPoplarCharacterSelectPhaseDefinition* waitNoAbandon = nullptr;
        UPoplarCharacterSelectPhaseDefinition* pick = nullptr;
        for (UPoplarCharacterSelectPhaseDefinition* p : SDKUtils::GetAllOfClass<UPoplarCharacterSelectPhaseDefinition>()) {
            if (!p) continue;
            std::string name = p->GetName();
            if (name == "PhaseDef_Setup_WaitingForPlayers_Abandon") waitAbandon = p;
            else if (name == "PhaseDef_Setup_WaitingForPlayers_NoAbandon") waitNoAbandon = p;
            else if (name == "PhaseDef_AllPlayers_Select") pick = p;
        }
        if (waitAbandon) waitAbandon->DurationOfPhase = kWaitingForPlayersSeconds;
        if (waitNoAbandon) waitNoAbandon->DurationOfPhase = kWaitingForPlayersSeconds;
        if (pick && randomHero) pick->DurationOfPhase = kRandomHeroPickSeconds;

        if (!waitAbandon || !waitNoAbandon || !pick)
            printf("[CHARSEL] rules: phase definitions not loaded (waiting-for-players %s/%s, pick %s)\n",
                waitAbandon ? "found" : "missing", waitNoAbandon ? "found" : "missing", pick ? "found" : "missing");
        printf("[CHARSEL] rules: waiting-for-players %.0f s, pick %.0f s\n", kWaitingForPlayersSeconds,
            pick ? pick->DurationOfPhase : -1.f);
        g_rulesApplied = true;
    }

    // How the game decides ownership (Ghidra, see the plan's research-findings.md, Q1):
    //  - Server: "does this player own hero X" is battleborn+0x1184e40 -> +0x112ff00, a scan of
    //    all 0x800 slots of the player's meta PRI StaticEntitlements for a non-null
    //    UPoplarMetaItemCharacterDefinition whose Character is X; a skin (and, by the same
    //    pattern, a taunt) is owned when its definition pointer is in the list (+0x1184d80).
    //    The apply-the-picked-hero step (+0x11963c0) falls back to the default hero
    //    (ModernSoldier) when that check fails. ReplicatedCharacterUsability is not read.
    //  - Client: the hero grid leaves its waiting state when +0x112f580 passes: the count of
    //    non-null StaticEntitlements (all slots) equals NumStaticEntitlements, the valid
    //    ReplicatedLoadouts equal NumLoadoutEntitlements, the GRI's usability entries are all
    //    there, and the player's PRI has CharacterSelectionState != 0. So every entry added here
    //    must be an object the client can resolve (a null on arrival keeps the grid shut), and
    //    NumStaticEntitlements must stay equal to the entries actually set.
    // The game (offline, no Hydra) already fills the list at login; this adds what it left out.
    void GrantEverything(APoplarPlayerController* pc) {
        APoplarPlayerReplicationInfo* pri = pc ? pc->MyPoplarPRI : nullptr;
        std::string name = pri ? pri->PlayerName.ToString() : std::string("?");
        APoplarMetaPlayerReplicationInfo* meta = pri ? pri->GetMetaPRI() : nullptr;
        if (!meta) {
            printf("[CHARSEL] %s has no meta PRI at login; granted 0 items\n", name.c_str());
            return;
        }

        std::unordered_set<UObject*> owned;
        int before = 0;
        for (int i = 0; i < 0x800; i++)
            if (meta->StaticEntitlements[i]) { owned.insert(meta->StaticEntitlements[i]); before++; }

        // Only objects the client's package map covers: anything else replicates as null. (The
        // server may have loaded a hero's skin package for an earlier player; a later player's
        // client has not.)
        UNetConnection* conn = reinterpret_cast<UNetConnection*>(pc->Player);
        int added[3] = { 0, 0, 0 };
        int notOnClient = 0;
        int noRoom = 0;
        int slot = 0;
        auto add = [&](UPoplarMetaItemDefinition* item, int kind) {
            if (!item || IsTemplate(item) || (item->ObjectFlags & kPendingKill) || owned.count(item)) return;
            if (!conn || !ServerNetworking::PackageMapSupportsObject(conn, item)) { notOnClient++; return; }
            while (slot < 0x800 && meta->StaticEntitlements[slot]) slot++;
            if (slot >= 0x800) { noRoom++; return; }
            meta->StaticEntitlements[slot] = item;
            owned.insert(item);
            added[kind]++;
        };
        for (UPoplarMetaItemCharacterDefinition* d : SDKUtils::GetAllOfClass<UPoplarMetaItemCharacterDefinition>()) add(d, 0);
        for (UPoplarMetaSkinDefinition* d : SDKUtils::GetAllOfClass<UPoplarMetaSkinDefinition>()) add(d, 1);
        for (UPoplarMetaTauntDefinition* d : SDKUtils::GetAllOfClass<UPoplarMetaTauntDefinition>()) add(d, 2);

        // The client's gate counts non-null slots, so the count is the slots in use (a repeated
        // item in the game's own list counts twice), never the number of distinct items.
        int granted = added[0] + added[1] + added[2];
        int total = before + granted;
        if (meta->NumStaticEntitlements != total) meta->NumStaticEntitlements = total;
        if (meta->ReceivedStaticEntitlements != total) meta->ReceivedStaticEntitlements = total;
        if (meta->NumLoadoutEntitlements < 0) meta->NumLoadoutEntitlements = 0;   // "none", so the client's loadout gate passes
        printf("[CHARSEL] %s granted %d items (heroes %d, skins %d, taunts %d); owns %d (had %d), %d left out (not in the client's package map), %d left out (no room), loadouts %d\n",
            name.c_str(), granted, added[0], added[1], added[2], total, before, notOnClient, noRoom, meta->NumLoadoutEntitlements);
    }

    APoplarGameReplicationInfo* LevelGRI() {
        APoplarGameReplicationInfo* gri = nullptr;
        for (APoplarGameReplicationInfo* g : SDKUtils::GetAllOfClass<APoplarGameReplicationInfo>())
            if (g && !(g->ObjectFlags & kPendingKill) && g->Outer && g->Outer->IsA(ULevel::StaticClass())) gri = g;
        return gri;
    }

    void LogState(APoplarGameReplicationInfo* gri) {
        // The line carries the phase's time left; it is printed when anything else changes
        // (`key` leaves the countdown out, so it does not print every second).
        char line[700] = "";
        std::string key;
        if (gri) {
            APoplarCharacterSelectManager* m = gri->CharacterSelectManager;
            int n = snprintf(line, sizeof line, "[CHARSEL] %s: match %d tournament %u manager %s",
                Globals::amServer ? "server" : "client", gri->CurrentMatchState.State, gri->CurrentMatchState.bTournamentMode,
                m ? "yes" : "none");
            key = line;
            if (m && !(m->ObjectFlags & kPendingKill)) {
                char teams[200];
                snprintf(teams, sizeof teams, " teamA %d/%d+%dbots teamB %d/%d+%dbots",
                    m->Teams[0].NumPlayers, m->Teams[0].NumExpectedPlayers, m->Teams[0].NumBots,
                    m->Teams[1].NumPlayers, m->Teams[1].NumExpectedPlayers, m->Teams[1].NumBots);
                char phase[300];
                snprintf(phase, sizeof phase, " state %d style %s phase#%d %s",
                    (int)m->CharacterSelectState, m->SelectionStyleDefinition ? m->SelectionStyleDefinition->GetName().c_str() : "none",
                    m->CurrentPhaseIndex, m->CurrentPhase ? m->CurrentPhase->GetName().c_str() : "none");
                n += snprintf(line + n, sizeof line - n, "%s time %.0f%s", phase, m->TimeRemainingInCurrentPhase, teams);
                key += std::string(phase) + teams;
            }
            if (!Globals::amServer) {
                APoplarPlayerController* pc = SDKUtils::GetLocalPlayerController();
                if (pc && !(pc->ObjectFlags & kPendingKill)) {
                    UPoplarCharacterSelectGFxMovie* mv = pc->CharacterSelectMovie;
                    APoplarPlayerReplicationInfo* pri = reinterpret_cast<APoplarPlayerReplicationInfo*>(pc->PlayerReplicationInfo);
                    char client[200];
                    snprintf(client, sizeof client, " | movie state %d movie %s ready %u ui %d pri-sel %d pawn %s",
                        (int)pc->CurrentCharacterSelectMovieState, mv ? "yes" : "no", mv ? mv->bMovieAndManagerReady : 0,
                        mv ? (int)mv->PlayerUIState[0] : -1, pri ? (int)pri->CharacterSelectionState : -1, pc->Pawn ? "yes" : "no");
                    n += snprintf(line + n, sizeof line - n, "%s", client);
                    key += client;
                }
            }
        }
        static std::string lastKey;
        if (*line && lastKey != key) { printf("%s\n", line); lastKey = key; }

        static bool dumped = false;
        if (Globals::amServer && gri && g_rulesApplied && !dumped) {
            dumped = true;
            DumpRules();
        }
    }

    namespace {
        // Latched, never reset: one server process hosts one match (ShutdownTimer ends it).
        bool g_phaseComplete = false;   // this match's pick phase has completed
        float g_sinceServerTick = 0.f;
        const float kLateJoinPickSeconds = 60.f;

        bool IsLive(UObject* o) { return o && !(o->ObjectFlags & kPendingKill); }

        APoplarCharacterSelectManager* LiveManager(APoplarGameReplicationInfo* gri) {
            APoplarCharacterSelectManager* m = gri ? gri->CharacterSelectManager : nullptr;
            return IsLive(m) ? m : nullptr;
        }

        UPoplarPlayerNameIdentifierDefinition* PickedHeroFrom(APoplarCharacterSelectManager* m, APoplarPlayerController* pc, const char** source) {
            const char* from = nullptr;
            UPoplarPlayerNameIdentifierDefinition* hero = nullptr;
            if (pc && pc->PendingPlayerClassNameId) {
                hero = pc->PendingPlayerClassNameId;
                from = "pending class";
            }
            APoplarPlayerReplicationInfo* pri = pc ? reinterpret_cast<APoplarPlayerReplicationInfo*>(pc->PlayerReplicationInfo) : nullptr;
            if (!hero && m && pri) {
                for (int t = 0; t < 2 && !hero; t++)
                    for (int i = 0; i < 5 && !hero; i++)
                        if (m->Teams[t].Players[i].PRI == pri && m->Teams[t].Players[i].SelectedCharacter) {
                            hero = m->Teams[t].Players[i].SelectedCharacter;
                            from = "team record";
                        }
                for (int i = 0; i < 0x1E && !hero; i++)
                    if (m->MasterCharacterList[i].SelectedBy == pri && m->MasterCharacterList[i].NameID) {
                        hero = m->MasterCharacterList[i].NameID;
                        from = "hero list SelectedBy";
                    }
            }
            if (source) *source = from;
            return hero;
        }

        // The player's live controller, or null while it is not logged in (or has left).
        APoplarPlayerController* LiveController(Globals::ServerPlayer& sp) {
            if (!sp.Connection || !ServerNetworking::IsLiveConnection(sp.Connection)) return nullptr;
            APlayerController* a = sp.Connection->Actor;
            if (!IsLive(a) || !a->IsA(APoplarPlayerController::StaticClass())) return nullptr;
            return reinterpret_cast<APoplarPlayerController*>(a);
        }

        void HandToSpawn(Globals::ServerPlayer& sp) {
            sp.SpawnScheduled = true;
            UNetConnection* c = sp.Connection;
            Engine::RunOnGameThreadAfter(0.5f, [c] { Hooks::SpawnPawnForServerPlayer(c, 1); });
        }
    }

    // The waiting room (Ghidra). A Setup phase with bMoveToNextPhaseWhenAllConnected ends, in its
    // done-test battleborn+0x11ee710, when
    //     manager.NumberOfExpectatedPlayerConnections (+0xC24)
    //         <= the team slots whose PRI has CurrentPlayerState >= Connected (3),
    // and otherwise when its time runs out (abandoning the match if bAbandonMatchIfTimeRunsOut).
    // WaitingForEntitlements (bMoveToNextPhaseWhenAllPlayersHaveEntitlements) ends when the slots
    // with bHasReceivedEntitlements == +0xC24, else on time. +0xC24 is set by +0x11e8610, the first
    // step of the manager's start-up (+0x16bdc40, also +0x1470ff0): it zeroes it and adds one per
    // player of the game's online player service (PoplarGameInfo.PlayerService); with no service
    // (this server) it stays 0, so "0 <= connected" ends the waiting room at once, and
    // Teams[t].NumExpectedPlayers is just the PRIs logged in at that moment. After it runs, the
    // manager expects the match's player count (ServerSettings::NumPlayersToStart).
    namespace {
        SafetyHookInline g_countExpectedPlayers;

        void CountExpectedPlayersHook(APoplarCharacterSelectManager* m) {
            g_countExpectedPlayers.call<void>(m);
            if (!Globals::amServer || !IsLive(m)) return;
            int want = (int)ServerSettings::NumPlayersToStart;
            int beforeConnections = m->NumberOfExpectatedPlayerConnections;
            int before[2] = { m->Teams[0].NumExpectedPlayers, m->Teams[1].NumExpectedPlayers };
            if (m->NumberOfExpectatedPlayerConnections < want) m->NumberOfExpectatedPlayerConnections = want;
            APoplarGameReplicationInfo* gri = m->WorldInfo ? reinterpret_cast<APoplarGameReplicationInfo*>(m->WorldInfo->GRI) : nullptr;
            bool pve = !gri || !gri->GameInfoDefinition || gri->GameInfoDefinition->bPvE;
            // Per team, for the screen and the bot count (+0x11e8e70 runs after this). PvE: all on
            // team A. PvP: fill whichever team expects fewer.
            while (m->Teams[0].NumExpectedPlayers + m->Teams[1].NumExpectedPlayers < want) {
                int t = pve ? 0 : (m->Teams[1].NumExpectedPlayers < m->Teams[0].NumExpectedPlayers ? 1 : 0);
                if (!pve && m->Teams[t].NumExpectedPlayers >= 5) t = 1 - t;
                if (m->Teams[t].NumExpectedPlayers >= 5) break;
                m->Teams[t].NumExpectedPlayers++;
            }
            printf("[CHARSEL] expecting %d players (%s): connections %d -> %d, team A %d -> %d, team B %d -> %d\n", want, pve ? "PvE" : "PvP",
                beforeConnections, m->NumberOfExpectatedPlayerConnections, before[0], m->Teams[0].NumExpectedPlayers,
                before[1], m->Teams[1].NumExpectedPlayers);
        }

        // The game's own "connected" count for the waiting room (see above).
        int ConnectedPlayers(APoplarCharacterSelectManager* m) {
            int n = 0;
            for (int t = 0; t < 2; t++)
                for (int i = 0; i < 5; i++) {
                    APoplarPlayerReplicationInfo* pri = m->Teams[t].Players[i].PRI;
                    if (pri && (uint8_t)pri->CurrentPlayerState >= (uint8_t)EPlayerState::PlayerState_Connected) n++;
                }
            return n;
        }

        bool IsWaitingRoom(UPoplarCharacterSelectPhaseDefinition* p) {
            return p && p->IsA(UPoplarCharacterSelectPhaseDefinition_Setup::StaticClass())
                && reinterpret_cast<UPoplarCharacterSelectPhaseDefinition_Setup*>(p)->bMoveToNextPhaseWhenAllConnected;
        }

        // Waiting room: logs "[CHARSEL] waiting room: N/M connected" on change. After it: the
        // players still missing will not come, so expect only those connected; otherwise
        // WaitingForEntitlements (which wants every expected player entitled) would add its full
        // 30 s for a no-show.
        void WatchWaitingRoom(APoplarCharacterSelectManager* m) {
            static int lastConnected = -1, lastExpected = -1;
            static bool waitingRoomOver = false;
            if (m->CharacterSelectState == ECharacterSelectManagerState::CSMS_Complete || !m->CurrentPhase || waitingRoomOver) return;
            int connected = ConnectedPlayers(m);
            int expected = m->NumberOfExpectatedPlayerConnections;
            if (IsWaitingRoom(m->CurrentPhase)) {
                if (connected != lastConnected || expected != lastExpected) {
                    lastConnected = connected;
                    lastExpected = expected;
                    printf("[CHARSEL] waiting room: %d/%d connected (%.0f s left)\n", connected, expected, m->TimeRemainingInCurrentPhase);
                }
                return;
            }
            // Past the waiting room (it is the style's first phase; this tick may not have seen it
            // at all when everyone was already there): everyone came, or the time ran out.
            waitingRoomOver = true;
            printf("[CHARSEL] waiting room: %d/%d connected (over; now %s)\n", connected, expected, m->CurrentPhase->GetName().c_str());
            if (expected > connected) {
                printf("[CHARSEL] waiting room over with %d of %d players; expecting only them from now on\n", connected, expected);
                m->NumberOfExpectatedPlayerConnections = connected;
            }
        }
    }

    void InstallServerHooks() {
        g_countExpectedPlayers = safetyhook::create_inline((void*)(Globals::baseAddress + 0x11e8610), &CountExpectedPlayersHook);
        if (!g_countExpectedPlayers)
            printf("[STARTUP] hook FAILED: character-select expected players (battleborn+0x11e8610)\n");
    }

    UPoplarPlayerNameIdentifierDefinition* PickedHero(APoplarPlayerController* pc, const char** source) {
        APoplarGameReplicationInfo* gri = pc && pc->WorldInfo ? reinterpret_cast<APoplarGameReplicationInfo*>(pc->WorldInfo->GRI) : nullptr;
        return PickedHeroFrom(LiveManager(gri), pc, source);
    }

    bool PhaseComplete() { return g_phaseComplete; }

    void OnServerTick(float dt) {
        g_sinceServerTick += dt;
        if (g_sinceServerTick < 1.f) return;
        float elapsed = g_sinceServerTick;
        g_sinceServerTick = 0.f;

        APoplarGameReplicationInfo* gri = LevelGRI();
        LogState(gri);
        APoplarCharacterSelectManager* m = LiveManager(gri);
        if (!m) return;
        WatchWaitingRoom(m);

        if (!g_phaseComplete) {
            if (m->CharacterSelectState != ECharacterSelectManagerState::CSMS_Complete) return;
            g_phaseComplete = true;
            Globals::haveHumansStarted = true;
            printf("[CHARSEL] pick phase complete; spawning the logged-in players\n");
            // Every player logged in now is dealt with here, once; anyone logged in later is a
            // late join (below).
            for (Globals::ServerPlayer& sp : Globals::ServerPlayers) {
                APoplarPlayerController* pc = LiveController(sp);
                if (!pc) continue;
                sp.SpawnScheduled = true;
                if (IsLive(pc->Pawn)) continue;
                const char* source = nullptr;
                if (UPoplarPlayerNameIdentifierDefinition* hero = PickedHeroFrom(m, pc, &source)) {
                    sp.Character = hero;
                    printf("[CHARSEL] %s locked in %s (from the %s)\n", sp.Name.c_str(), hero->GetName().c_str(), source);
                }
                else {
                    printf("[CHARSEL] %s has no recorded pick; the game's random rule should have assigned one\n", sp.Name.c_str());
                }
                HandToSpawn(sp);
            }
            return;
        }

        // Late join: a player logged in after the phase completed gets the hero screen; once
        // their lock-in is recorded, spawn them. With no pick after kLateJoinPickSeconds, spawn
        // them anyway with whatever class the game has pending (the helper copes with no hero).
        for (Globals::ServerPlayer& sp : Globals::ServerPlayers) {
            if (sp.SpawnScheduled) continue;
            APoplarPlayerController* pc = LiveController(sp);
            if (!pc || IsLive(pc->Pawn)) continue;
            const char* source = nullptr;
            UPoplarPlayerNameIdentifierDefinition* hero = PickedHeroFrom(m, pc, &source);
            if (hero) {
                sp.Character = hero;
                printf("[CHARSEL] late join: %s picked %s (from the %s, after %.0f s)\n", sp.Name.c_str(), hero->GetName().c_str(), source, sp.NoPickSeconds);
                HandToSpawn(sp);
                continue;
            }
            sp.NoPickSeconds += elapsed;
            if (sp.NoPickSeconds >= kLateJoinPickSeconds) {
                printf("[CHARSEL] late join: %s made no pick in %.0f s; spawning with the game's class\n", sp.Name.c_str(), kLateJoinPickSeconds);
                HandToSpawn(sp);
            }
        }
    }
}

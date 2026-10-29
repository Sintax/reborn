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
                printf("[CHARSEL] style %s code \"%s\" \"%s\" unique %d botpick %.0fs blind %u pve %u 5v5 %u 3v3 %u: %s\n", s->GetName().c_str(),
                    s->CodeName.ToString().c_str(), s->StyleName.ToString().c_str(), (int)s->UniquenessLevel, s->SimulatedBotSelectionTime, s->bBlindSelection,
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

    void ApplyServerRules(APoplarGameInfo* gameInfo, bool randomHero) {
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

        // Draft style: the game's InitGame (0x1474140) reads ?DraftStyle=<CodeName> and ?DraftStyleIndex=<n>
        // from the map URL itself, so the server only passes the option on (no ApplyDraftStyle needed).
        if (gameInfo)
            printf("[CHARSEL] draft style \"%s\" index %d (from the map URL; empty = the game's default)\n",
                gameInfo->CharacterSelectStyleOptionString.ToString().c_str(), gameInfo->CharacterSelectStyleOption);
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
        // Fast path: the world's own GRI, reached through a live controller. The full object walk
        // below cost ~10 ms each call (run 20261029-030612) and is kept only as the fallback.
        AWorldInfo* wi = nullptr;
        if (APoplarPlayerController* pc = SDKUtils::GetLocalPlayerController())
            wi = pc->WorldInfo;
        if (!wi && Globals::amServer) {
            for (Globals::ServerPlayer& sp : Globals::ServerPlayers) {
                if (!sp.Connection) continue;
                APlayerController* a = sp.Connection->Actor;
                if (a && !(a->ObjectFlags & kPendingKill) && a->WorldInfo) { wi = a->WorldInfo; break; }
            }
        }
        if (wi && !(wi->ObjectFlags & kPendingKill)) {
            AGameReplicationInfo* g = wi->GRI;
            if (g && !(g->ObjectFlags & kPendingKill) && g->IsA(APoplarGameReplicationInfo::StaticClass())
                && g->Outer && g->Outer->IsA(ULevel::StaticClass()))
                return reinterpret_cast<APoplarGameReplicationInfo*>(g);
        }

        APoplarGameReplicationInfo* gri = nullptr;
        for (APoplarGameReplicationInfo* g : SDKUtils::GetAllOfClass<APoplarGameReplicationInfo>())
            if (g && !(g->ObjectFlags & kPendingKill) && g->Outer && g->Outer->IsA(ULevel::StaticClass())) gri = g;
        return gri;
    }

    // [INTRO] client, on change: the hero intro as this client sees it. The coordinator registers
    // itself in PoplarGlobals.CharacterIntroCoordinator (GWorld+0x1a8 is the PoplarGlobals the
    // game's own coordinator code uses, battleborn+0x1358cb0), so no object walk is needed.
    // Client state machine (Ghidra, table built at battleborn+0x13572f0): LoadingData tick
    // +0x1357a90 waits for the local select movie (bMovieAndManagerReady) and, per player, an
    // initialized entry whose stand-in has its meshes; entries are initialized by +0x1357770 from
    // PoplarGlobals.AssignedStandInSets[1] (the intro stand-ins placed in the map).
    static void LogClientIntro(APoplarPlayerController* pc) {
        UWorld* world = Globals::GetGWorld();
        if (!world) return;
        UObject* g = *reinterpret_cast<UObject**>(reinterpret_cast<uintptr_t>(world) + 0x1a8);
        if (!g || !g->IsA(UPoplarGlobals::StaticClass())) return;
        UPoplarGlobals* pg = reinterpret_cast<UPoplarGlobals*>(g);
        APoplarCharacterIntroCoordinator* c = pg->CharacterIntroCoordinator;
        char line[1200];
        int n = snprintf(line, sizeof line, "[INTRO] client: coordinator %s", c ? (c->ObjectFlags & kPendingKill ? "dying" : "yes") : "none");
        uintptr_t cb = reinterpret_cast<uintptr_t>(c);
        if (c && !(c->ObjectFlags & kPendingKill)) {
            UPoplarCharacterIntroDefinition* def = c->CharacterIntroDefinition;
            n += snprintf(line + n, sizeof line - n, " (state %d, wants %d, %d players, elapsed %.0f, hero %d, def %s, role %d)",
                (int)c->CurrentState, (int)c->DesiredState, c->NumPlayersToIntroduce, c->ElapsedTime, c->CurrentCharacterIndex,
                def ? def->GetName().c_str() : "none", (int)c->Role);
            for (int i = 0; i < 10 && i < c->NumPlayersToIntroduce + 2; i++) {
                uintptr_t e = cb + 0x240 + i * 0x30;
                UObject* pri = *reinterpret_cast<UObject**>(e);
                UObject* taunt = *reinterpret_cast<UObject**>(e + 0x8);
                UObject* nameId = *reinterpret_cast<UObject**>(e + 0x10);
                UObject* standIn = *reinterpret_cast<UObject**>(e + 0x20);
                uint32_t init = *reinterpret_cast<uint32_t*>(e + 0x28) & 1;
                if (!pri && !taunt && !nameId && !standIn && !init) continue;
                bool meshes = standIn && *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(standIn) + 0x380) && *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(standIn) + 0x338);
                n += snprintf(line + n, sizeof line - n, "; [%d] pri %s taunt %s hero %s standin %s meshes %s init %u", i,
                    pri ? "yes" : "none", taunt ? taunt->GetName().c_str() : "none", nameId ? nameId->GetName().c_str() : "none",
                    standIn ? standIn->GetName().c_str() : "none", meshes ? "yes" : "no", init);
            }
        }
        int registered = pg->RegisteredStandIns.ArrayCount;
        int introSlots = 0;
        for (int i = 0; i < 10; i++) if (pg->AssignedStandInSets[1].AssignedStandIns[i].StandIn) introSlots++;
        UPoplarCharacterSelectGFxMovie* mv = pc ? pc->CharacterSelectMovie : nullptr;
        n += snprintf(line + n, sizeof line - n, " | stand-ins registered %d, intro slots %d, select movie %s ready %u, displaying intros %u",
            registered, introSlots, mv ? "yes" : "no", mv ? mv->bMovieAndManagerReady : 0, pc ? pc->bDisplayingCharacterIntros : 0);
        if (pc) {
            ACamera* cam = pc->PlayerCamera;
            AActor* vt = cam ? cam->ViewTarget.Target : nullptr;
            n += snprintf(line + n, sizeof line - n, ", view target %s, pc state %s", vt ? vt->GetName().c_str() : "none",
                pc->GetStateName().ToString().c_str());
        }
        // Not registered: is there a coordinator object on this client at all? (One object walk,
        // at most 3 times per process, only while the server's coordinator should be running.)
        static int walks = 0;
        static int callsSinceWalk = 100;   // called once a second
        callsSinceWalk++;
        if (!c && walks < 3 && pc && pc->WorldInfo && pc->WorldInfo->GRI
            && reinterpret_cast<APoplarGameReplicationInfo*>(pc->WorldInfo->GRI)->CurrentMatchState.State == 3
            && callsSinceWalk >= 5) {
            walks++;
            callsSinceWalk = 0;
            auto all = SDKUtils::GetAllOfClass<APoplarCharacterIntroCoordinator>();
            std::string found;
            for (APoplarCharacterIntroCoordinator* o : all) {
                if (!o) continue;
                char b[200];
                snprintf(b, sizeof b, " %s(state %d wants %d n %d role %d)", o->GetFullName().c_str(), (int)o->CurrentState, (int)o->DesiredState, o->NumPlayersToIntroduce, (int)o->Role);
                found += b;
            }
            printf("[INTRO] client: object walk %d at match 3 found %zu coordinator objects:%s\n", walks, all.size(), found.c_str());
        }
        static std::string last;
        if (last != line) {
            SYSTEMTIME st; GetLocalTime(&st);
            printf("%s (at %02d:%02d:%02d)\n", line, st.wHour, st.wMinute, st.wSecond);
            last = line;
        }
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
                    if (gri->CurrentMatchState.State >= 2 && gri->CurrentMatchState.State <= 6)
                        LogClientIntro(pc);
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
        // A late joiner gets no hero screen (see EarlierHero's note), so there is nothing to wait
        // for beyond the game's default pending class, which arrives about 1 s after the login.
        const float kLateJoinSettleSeconds = 3.f;

        bool IsLive(UObject* o) { return o && !(o->ObjectFlags & kPendingKill); }

        APoplarCharacterSelectManager* LiveManager(APoplarGameReplicationInfo* gri) {
            APoplarCharacterSelectManager* m = gri ? gri->CharacterSelectManager : nullptr;
            return IsLive(m) ? m : nullptr;
        }

        // usePending false: only what the hero screen recorded (a late joiner's pending class is just
        // the controller's default, not a pick).
        UPoplarPlayerNameIdentifierDefinition* PickedHeroFrom(APoplarCharacterSelectManager* m, APoplarPlayerController* pc, const char** source, bool usePending = true) {
            const char* from = nullptr;
            UPoplarPlayerNameIdentifierDefinition* hero = nullptr;
            if (usePending && pc && pc->PendingPlayerClassNameId) {
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

        // A player who joins after the pick phase (Ghidra): the game's PostLogin (battleborn+0x1481ba0)
        // adds new humans to the select manager (+0x11e9620 -> +0x11ea610) only while the manager is
        // InProgress, so a late joiner gets no team record, no SelectedBy, no hero screen (the client's
        // movie stays Closed) and only the controller's default pending class (Oscar Mike, seen in
        // run 20261018-214639). A player who drops and rejoins arrives the same way, as a new
        // ServerPlayer; the old one stays in the list with no connection.

        // The hero this player had before dropping out: an earlier ServerPlayer with no connection
        // that asked for the same name (WantedName comes with the join, before the login names it).
        UPoplarPlayerNameIdentifierDefinition* EarlierHero(const Globals::ServerPlayer& sp) {
            const std::string& me = sp.WantedName.empty() ? sp.Name : sp.WantedName;
            UPoplarPlayerNameIdentifierDefinition* hero = nullptr;
            for (const Globals::ServerPlayer& other : Globals::ServerPlayers) {
                if (&other == &sp || other.Connection || !other.Character) continue;
                const std::string& them = other.WantedName.empty() ? other.Name : other.WantedName;
                if (them == me) hero = other.Character;   // the latest one wins
            }
            return hero;
        }

        // Always a hero for a player the game recorded no pick for: the game's pending class, else
        // a hero no other connected player has, else the first hero in the game's list.
        UPoplarPlayerNameIdentifierDefinition* FallbackHero(APoplarCharacterSelectManager* m, APoplarPlayerController* pc, const char** source) {
            if (pc && pc->PendingPlayerClassNameId) {
                *source = "pending class";
                return pc->PendingPlayerClassNameId;
            }
            UPoplarPlayerNameIdentifierDefinition* first = nullptr;
            for (int i = 0; m && i < 0x1E; i++) {
                UPoplarPlayerNameIdentifierDefinition* id = m->MasterCharacterList[i].NameID;
                if (!IsLive(id)) continue;
                if (!first) first = id;
                bool held = false;
                for (Globals::ServerPlayer& other : Globals::ServerPlayers)
                    if (other.Character == id && other.Connection && ServerNetworking::IsLiveConnection(other.Connection)) held = true;
                if (!held) {
                    *source = "first hero no one holds";
                    return id;
                }
            }
            *source = "first hero in the list";
            return first;
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

    bool TimerPicksOnly() { return ServerSettings::RandomHero && !g_phaseComplete; }

    // [INTRO] server, on change: the hero intro story games show after the pick (Ghidra: the
    // PlayerSetup start, battleborn+0x147b040, spawns PoplarGameInfo.CharacterIntroCoordinator for
    // the players with a hero and a taunt; while it runs the clients keep the select movie up).
    // Diagnostic: tells whether the game type has an intro, whether a coordinator exists and how
    // far it gets, and each player's hero and taunt.
    static void LogIntro(APoplarGameReplicationInfo* gri) {
        if (!gri || !gri->WorldInfo || !gri->WorldInfo->Game
            || !gri->WorldInfo->Game->IsA(APoplarGameInfo::StaticClass())) return;
        APoplarGameInfo* gi = reinterpret_cast<APoplarGameInfo*>(gri->WorldInfo->Game);   // no object walk: ~10 ms each
        if (!IsLive(gi)) return;
        UPoplarGameInfoDefinition* def = gri->GameInfoDefinition;
        APoplarCharacterIntroCoordinator* c = gi->CharacterIntroCoordinator;
        char line[600];
        int n = snprintf(line, sizeof line, "[INTRO] server: match %d, game type %s, intro %s, coordinator %s",
            gri->CurrentMatchState.State, def ? def->GetName().c_str() : "none",
            def && def->CharacterIntroDefinition ? def->CharacterIntroDefinition->GetName().c_str() : "none",
            IsLive(c) ? "yes" : "none");
        if (IsLive(c))
            n += snprintf(line + n, sizeof line - n, " (state %d, wants %d, %d players, %d ready, hero %d, elapsed %.0f, movie %.1f s + %.1f s each)", (int)c->CurrentState,
                (int)c->DesiredState, c->NumPlayersToIntroduce, c->PlayersWaitingToStartCharacterIntros.ArrayCount, c->CurrentCharacterIndex,
                c->ElapsedTime, c->CharacterIntroDefinition ? c->CharacterIntroDefinition->IntroMovieLength : -1.f,
                c->CharacterIntroDefinition ? c->CharacterIntroDefinition->PerCharacterDisplayTime : -1.f);
        if (IsLive(c)) {
            // Does each client get the coordinator? Channel per connection and the class in its package map.
            n += snprintf(line + n, sizeof line - n, " rep: role %d/%d hidden %u", (int)c->Role, (int)c->RemoteRole, (unsigned)c->bHidden);
            for (Globals::ServerPlayer& sp : Globals::ServerPlayers) {
                if (!sp.Connection || !ServerNetworking::IsLiveConnection(sp.Connection)) continue;
                UActorChannel* ch = ServerNetworking::GetActorChannelForActor(c, sp.Connection);
                n += snprintf(line + n, sizeof line - n, ", %s: class in map %s channel %s", sp.Name.c_str(),
                    ServerNetworking::PackageMapSupportsObject(sp.Connection, c->Class) ? "yes" : "NO",
                    ch ? (std::string("idx ") + std::to_string(ch->ChIndex) + " openAckd " + std::to_string(ch->OpenAckd)).c_str() : "NONE");
            }
        }
        std::string players;
        for (Globals::ServerPlayer& sp : Globals::ServerPlayers) {
            APoplarPlayerController* pc = LiveController(sp);
            APoplarPlayerReplicationInfo* pri = pc ? reinterpret_cast<APoplarPlayerReplicationInfo*>(pc->PlayerReplicationInfo) : nullptr;
            if (!IsLive(pri)) continue;
            APoplarMetaPlayerReplicationInfo* meta = pri->GetMetaPRI();
            players += "; " + sp.Name + ": hero " + (pri->CharacterNameIdDef ? pri->CharacterNameIdDef->GetName() : std::string("none"))
                + ", browsing " + (pri->BrowseCharacterNameIdDef ? pri->BrowseCharacterNameIdDef->GetName() : std::string("none"))
                + ", taunt " + (IsLive(meta) && meta->CurrentTaunt ? meta->CurrentTaunt->GetName() : std::string(IsLive(meta) ? "none" : "no meta PRI"));
        }
        std::string out = std::string(line) + players;
        static std::string last;
        if (out != last) { printf("%s\n", out.c_str()); last = out; }
    }

    void OnServerTick(float dt) {
        g_sinceServerTick += dt;
        if (g_sinceServerTick < 1.f) return;
        float elapsed = g_sinceServerTick;
        g_sinceServerTick = 0.f;

        APoplarGameReplicationInfo* gri = LevelGRI();
        LogState(gri);
        LogIntro(gri);
        ServerNetworking::CatchUpPackageMapsForQueuedBots();
        APoplarCharacterSelectManager* m = LiveManager(gri);
        if (!m) return;

        // Abandon (Ghidra): a waiting room with bAbandonMatchIfTimeRunsOut (the Versus one) that
        // times out short of players ends the match itself (+0x11ee710 -> the game's end-match
        // call with outcome TooFewPlayers), so the match is Ended before the pick phase completes.
        // The game sends the players home; end the server game as at a normal match end.
        if (!g_phaseComplete && gri->CurrentMatchState.State >= 7 /* MatchState_Ended */) {
            if (Globals::ShutdownTimer == 0.0f) {
                printf("[CHARSEL] match abandoned: not enough players (match %d, outcome %d, %d/%d connected)\n",
                    gri->CurrentMatchState.State, (int)gri->CurrentMatchState.Outcome, ConnectedPlayers(m),
                    m->NumberOfExpectatedPlayerConnections);
                printf("[GAME] Match ended (abandoned), shutting down in 10sec!\n");
                Globals::ShutdownTimer = 10.0f;
            }
            return;
        }
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
                else if (UPoplarPlayerNameIdentifierDefinition* fallback = FallbackHero(m, pc, &source)) {
                    // Logged in during "Prepare for Battle" but not in a team slot: no pick recorded.
                    sp.Character = fallback;
                    printf("[CHARSEL] %s has no recorded pick; spawning as %s (fallback, from the %s)\n", sp.Name.c_str(), fallback->GetName().c_str(), source);
                }
                else {
                    printf("[CHARSEL] %s has no recorded pick and the game lists no heroes; spawning with the game's class\n", sp.Name.c_str());
                }
                HandToSpawn(sp);
            }
            return;
        }

        // Late join (and rejoin): a player logged in after the phase completed. The game records
        // no pick for them, so: the hero they had before a drop, else, after a short settle, a
        // concrete fallback hero (FallbackHero: the game's pending class first).
        for (Globals::ServerPlayer& sp : Globals::ServerPlayers) {
            if (sp.SpawnScheduled) continue;
            APoplarPlayerController* pc = LiveController(sp);
            if (!pc || IsLive(pc->Pawn)) continue;
            const char* source = nullptr;
            UPoplarPlayerNameIdentifierDefinition* hero = PickedHeroFrom(m, pc, &source, false);
            if (!hero && (hero = EarlierHero(sp))) source = "hero they had before leaving";
            if (hero) {
                sp.Character = hero;
                printf("[CHARSEL] late join: %s picked %s (from the %s, after %.0f s)\n", sp.Name.c_str(), hero->GetName().c_str(), source, sp.NoPickSeconds);
                HandToSpawn(sp);
                continue;
            }
            sp.NoPickSeconds += elapsed;
            if (sp.NoPickSeconds < kLateJoinSettleSeconds) continue;
            if ((hero = FallbackHero(m, pc, &source))) {
                sp.Character = hero;
                printf("[CHARSEL] late joiner %s spawned as %s (fallback), from the %s\n", sp.Name.c_str(), hero->GetName().c_str(), source);
            }
            else {
                printf("[CHARSEL] late joiner %s: the game lists no heroes; spawning with the game's class\n", sp.Name.c_str());
            }
            HandToSpawn(sp);
        }
    }

    // PoplarPlayerController.ECharacterSelectMovieState (uescript-bb): Closed, LoadingLevel,
    // InProgress, ClashScreen, Closing.
    void UpdateGearBox() {
        bool open = false;
        if (!Globals::amServer && !Globals::amStandalone) {
            APoplarPlayerController* pc = SDKUtils::GetLocalPlayerController();
            open = IsLive(pc) && pc->CurrentCharacterSelectMovieState == ECharacterSelectMovieState::CharacterSelectMovie_InProgress;
        }
        if (open != Globals::GearBoxOpen) printf("[CHARSEL] gear box %s\n", open ? "shown" : "hidden");
        Globals::GearBoxOpen = open;
    }
}

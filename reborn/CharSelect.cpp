#include "CharSelect.hpp"
#include "Globals.hpp"
#include "Utils.hpp"
#include <cstdio>
#include <string>

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

    APoplarGameReplicationInfo* LevelGRI() {
        APoplarGameReplicationInfo* gri = nullptr;
        for (APoplarGameReplicationInfo* g : SDKUtils::GetAllOfClass<APoplarGameReplicationInfo>())
            if (g && !(g->ObjectFlags & kPendingKill) && g->Outer && g->Outer->IsA(ULevel::StaticClass())) gri = g;
        return gri;
    }

    void LogState() {
        APoplarGameReplicationInfo* gri = LevelGRI();
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
}

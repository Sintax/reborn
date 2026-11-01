#pragma once
#include <string>

// How long the hero screen's timers run on our server, and the Ready shortcut. Pure so it can be
// unit tested (tests/test_char_select_times.cpp).
namespace CharSelectTimes {
    // Seconds left when everyone has pressed Ready: a short count instead of a sudden cut.
    constexpr float kReadyLeftSeconds = 3.f;

    // The length the server gives a phase, or 0 to keep the game's own. Story and Operations get
    // more time than Versus (owner's choice 2026-11-01: 3 min vs 2 min 15 s before the match).
    inline float PhaseSeconds(const std::string& phaseName, bool pve) {
        if (phaseName == "PhaseDef_AllPlayers_Select") return pve ? 120.f : 90.f;    // pick a hero
        if (phaseName == "PhaseDef_Setup_PostSelection") return pve ? 60.f : 45.f;   // get ready
        return 0.f;
    }

    // The phases Ready can cut short.
    inline bool ReadyCanSkip(const std::string& phaseName) {
        return phaseName == "PhaseDef_AllPlayers_Select" || phaseName == "PhaseDef_Setup_PostSelection";
    }

    // Everyone counted is ready: at least one player, and every one of them.
    inline bool EveryoneReady(int players, int ready) { return players > 0 && ready >= players; }

    // The time left after a phase is lengthened, given how long it has already run.
    inline float LengthenedTimeLeft(float target, float gameLength, float timeLeft) {
        float ran = gameLength - timeLeft;
        if (ran < 0.f) ran = 0.f;
        float left = target - ran;
        return left < 1.f ? 1.f : left;
    }
}

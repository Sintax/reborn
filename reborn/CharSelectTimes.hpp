#pragma once
#include <cstdio>
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

    // The server's ready count, sent to every player as control text "RebornReady=<ready>/<players>".
    inline std::string ReadyCountText(int ready, int players) {
        return "RebornReady=" + std::to_string(ready) + "/" + std::to_string(players);
    }

    // Reads ReadyCountText; false (outputs untouched) for any other text.
    inline bool ParseReadyCount(const std::string& s, int& ready, int& players) {
        const std::string tag = "RebornReady=";
        if (s.rfind(tag, 0) != 0) return false;
        int r = 0, p = 0;
        if (std::sscanf(s.c_str() + tag.size(), "%d/%d", &r, &p) != 2 || r < 0 || p < 0 || r > p || p > 10) return false;
        ready = r;
        players = p;
        return true;
    }

    // The Ready window's line. players 0 = no count from the server yet.
    inline std::string ReadyStatus(bool meReady, bool picked, int ready, int players) {
        if (!meReady && !picked) return "Pick your hero first.";
        if (players <= 0) return meReady ? "You're ready. The match starts when everyone is." : "Press Ready when you're set.";
        int waiting = players - ready;
        if (meReady) {
            if (waiting <= 0) return "Everyone is ready. Starting...";
            return "You're ready. Waiting for " + std::to_string(waiting) + (waiting == 1 ? " more player." : " more players.");
        }
        return std::to_string(ready) + " of " + std::to_string(players) + " players ready. Press Ready when you're set.";
    }

    // The time left after a phase is lengthened, given how long it has already run.
    inline float LengthenedTimeLeft(float target, float gameLength, float timeLeft) {
        float ran = gameLength - timeLeft;
        if (ran < 0.f) ran = 0.f;
        float left = target - ran;
        return left < 1.f ? 1.f : left;
    }
}

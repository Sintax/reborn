// reborn/tests/test_char_select_times.cpp
#include "../CharSelectTimes.hpp"
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

int main() {
    using namespace CharSelectTimes;
    // Story/Operations: 2 min pick + 1 min get-ready; Versus: 90 s + 45 s.
    CHECK(PhaseSeconds("PhaseDef_AllPlayers_Select", true) == 120.f);
    CHECK(PhaseSeconds("PhaseDef_Setup_PostSelection", true) == 60.f);
    CHECK(PhaseSeconds("PhaseDef_AllPlayers_Select", false) == 90.f);
    CHECK(PhaseSeconds("PhaseDef_Setup_PostSelection", false) == 45.f);
    // Other phases (waiting room, bans, captains) keep the game's own length.
    CHECK(PhaseSeconds("PhaseDef_Setup_WaitingForPlayers_Abandon", false) == 0.f);
    CHECK(PhaseSeconds("PhaseDef_TeamA_VoteToBan1", false) == 0.f);

    CHECK(ReadyCanSkip("PhaseDef_AllPlayers_Select") && ReadyCanSkip("PhaseDef_Setup_PostSelection"));
    CHECK(!ReadyCanSkip("PhaseDef_Setup_WaitingForEntitlements"));

    CHECK(EveryoneReady(2, 2) && EveryoneReady(1, 1));
    CHECK(!EveryoneReady(3, 2));
    CHECK(!EveryoneReady(0, 0));   // nobody there: never "everyone ready"

    // Seen 1 s into a 60 s phase, lengthened to 120: 119 left. Never below 1 s.
    CHECK(LengthenedTimeLeft(120.f, 60.f, 59.f) == 119.f);
    CHECK(LengthenedTimeLeft(120.f, 60.f, 60.f) == 120.f);
    CHECK(LengthenedTimeLeft(45.f, 40.f, 40.5f) == 45.f);   // a timer read above its length
    CHECK(LengthenedTimeLeft(5.f, 60.f, 0.f) == 1.f);

    // The ready count travels as "RebornReady=<ready>/<players>".
    int r = -1, p = -1;
    CHECK(ReadyCountText(1, 3) == "RebornReady=1/3");
    CHECK(ParseReadyCount("RebornReady=1/3", r, p) && r == 1 && p == 3);
    CHECK(!ParseReadyCount("RebornRefused=Improper Credentials!", r, p) && r == 1);   // other text: untouched
    CHECK(!ParseReadyCount("RebornReady=4/3", r, p));    // more ready than players
    CHECK(!ParseReadyCount("RebornReady=x/3", r, p));

    // The window's line.
    CHECK(ReadyStatus(false, false, 0, 2) == "Pick your hero first.");
    CHECK(ReadyStatus(false, true, 1, 3) == "1 of 3 players ready. Press Ready when you're set.");
    CHECK(ReadyStatus(true, true, 1, 3) == "You're ready. Waiting for 2 more players.");
    CHECK(ReadyStatus(true, true, 2, 3) == "You're ready. Waiting for 1 more player.");
    CHECK(ReadyStatus(true, true, 3, 3) == "Everyone is ready. Starting...");
    CHECK(ReadyStatus(true, true, 0, 0) == "You're ready. The match starts when everyone is.");   // no count yet

    if (failures == 0) std::printf("PASS\n");
    return failures == 0 ? 0 : 1;
}

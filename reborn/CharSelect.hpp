#pragma once

#include "CharSelectRules.hpp"

class APoplarGameReplicationInfo;

// The game's own character-select phase, which every networked game runs (the server's map string
// carries ?bTournamentMode=1, see WithTournamentMode).
namespace CharSelect {
    // Server, once the match's game info is set up: give players 120 s to arrive in the waiting
    // room; with randomHero the pick phase lasts 5 s instead of the game's 60 s.
    void ApplyServerRules(bool randomHero);

    // The live (not pending-kill) replication info of the loaded level, or null.
    APoplarGameReplicationInfo* LevelGRI();

    // Called about once a second from the tick: prints the [CHARSEL] state line when the state
    // changes, and on the server dumps the select rules once after ApplyServerRules ran.
    void LogState();
}

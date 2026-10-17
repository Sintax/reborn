#pragma once

#include "CharSelectRules.hpp"

class APoplarGameReplicationInfo;
class APoplarPlayerController;
class UPoplarPlayerNameIdentifierDefinition;

// The game's own character-select phase, which every networked game runs (the server's map string
// carries ?bTournamentMode=1, see WithTournamentMode).
namespace CharSelect {
    // Server, once the match's game info is set up: give players 120 s to arrive in the waiting
    // room; with randomHero the pick phase lasts 5 s instead of the game's 60 s.
    void ApplyServerRules(bool randomHero);

    // Server, right after a player's PostLogin: the player owns every hero, skin and taunt (adds
    // the missing ones to the player's meta PRI StaticEntitlements, which the server's ownership
    // check and the client's hero grid both read; only objects the client's package map covers).
    // Logs "[CHARSEL] <name> granted <n> items".
    void GrantEverything(APoplarPlayerController* pc);

    // The live (not pending-kill) replication info of the loaded level, or null.
    APoplarGameReplicationInfo* LevelGRI();

    // Called about once a second with the level's GRI (LevelGRI()): prints the [CHARSEL] state
    // line when the state changes, and on the server dumps the select rules once after
    // ApplyServerRules ran.
    void LogState(APoplarGameReplicationInfo* gri);

    // Server: the hero the game recorded for this player, or null if none. In order: the
    // controller's PendingPlayerClassNameId; the select manager's team record
    // (Teams[t].Players[i].SelectedCharacter for the player's PRI); the hero list entry the player
    // holds (MasterCharacterList[i].SelectedBy, which the server sets on the confirmed click while
    // the PRI's selection state stays Browsing until the phase ends). `source`, when given, is set
    // to which of the three it came from.
    UPoplarPlayerNameIdentifierDefinition* PickedHero(APoplarPlayerController* pc, const char** source = nullptr);

    // Server: true once the match's pick phase has completed (the manager reached Complete).
    bool PhaseComplete();

    // Server, every tick (does its work about once a second; one object-list walk per run):
    // logs the [CHARSEL] state; when the manager becomes Complete, sets
    // Globals::haveHumansStarted, logs "[CHARSEL] <name> locked in <hero>" for every logged-in
    // player without a pawn and hands each one to Hooks::SpawnPawnForServerPlayer. After that, a
    // player logged in later is handed over once the game has a pick for them
    // ("[CHARSEL] late join: <name> picked <hero>").
    void OnServerTick(float dt);
}

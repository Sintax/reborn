#pragma once

#include "PlayerNameRules.hpp"

#include <string>

class APlayerController;
class UFunction;
class UObject;

// The names humans show on the hero screen, the scoreboard and in [TEAMS].
// The game asks Steam for a player's name by its id and overwrites the name the server sends;
// the mod's players have made-up ids, so the Steam emulator answered "Unknown User". The refresh
// hook keeps the server's name for humans in network games instead, and each client sends the
// name it chose (reborn-player.txt) once it has joined.
// Not done yet: PlayerName reaches a client only with the PRI's first update, so a client that
// already has a player's PRI keeps showing "LAN Player" for it after the server renames it.
namespace PlayerNames {
    // Client and server, at startup: hook the game's player-name refresh (battleborn+0x123ac90).
    void InstallHooks();

    // The name this player chose: -rbname (test players only) or reborn-player.txt, cleaned.
    std::string ChosenName();

    // Client, every tick: once joined, send ChosenName to the server (ServerChangeName) once.
    // Logs "[NAMES] client: players ..." when the names it sees change.
    void ClientTick(float dt);

    // Server: the one place a human's name is set. Cleans it, adds " 2", " 3"... when another
    // human has it, sets the scoreboard name and the ServerPlayer name, logs "[NAMES] ...".
    void SetHumanName(APlayerController* pc, const std::string& wanted);

    // Server, from ProcessEvent: true when this was a client's ServerChangeName, now handled
    // (the game's own handler is skipped).
    bool OnServerChangeName(UObject* object, UFunction* function, void* params);
}

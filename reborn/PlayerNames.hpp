#pragma once

#include "PlayerNameRules.hpp"

#include <string>

class APlayerController;
class UNetConnection;

// The names humans show on the hero screen, the scoreboard and in [TEAMS].
// The game asks Steam for a player's name by its id and overwrites the name the server sends;
// the mod's players have made-up ids, so the Steam emulator answered "Unknown User". The refresh
// hook keeps the server's name for humans in network games instead.
// PlayerName reaches clients only with a PRI's first update, so the name travels with the join:
// the client sends it while joining (SendJoinName), the server keeps it (OnJoinText) and puts it
// on the PRI at login, before the PRI is first sent (SetHumanName).
namespace PlayerNames {
    // Client and server, at startup: hook the game's player-name refresh (battleborn+0x123ac90).
    // Client also: hook the joining handler (battleborn+0x4c2000) so that, once the server has
    // welcomed us, "RebornName=<ChosenName>" goes to it as an NMT_DebugText control message. It
    // arrives before NMT_Join, so before the login.
    void InstallHooks();

    // The name this player chose: -rbname (test players only) or reborn-player.txt, cleaned.
    std::string ChosenName();

    // Server, on NMT_DebugText: reads the text; "RebornName=..." is kept as the player's
    // WantedName. Logs "[NAMES] ...".
    void OnJoinText(UNetConnection* connection, void* inbunch);

    // Client, every tick in a network game: logs "[NAMES] client: players ..." when the names it
    // sees change.
    void ClientTick(float dt);

    // Server: the one place a human's name is set. Cleans it, adds " 2", " 3"... when another
    // human has it, sets the scoreboard name and the ServerPlayer name, logs "[NAMES] ...".
    // Called at login, before the PRI's first replication (later changes do not reach clients).
    void SetHumanName(APlayerController* pc, std::string wanted);
}

#pragma once

#include <string>

namespace Globals { struct ServerPlayer; }

// Who may join this server: the user list (reborn-users.json) checked against the name and key
// each player sends while joining (PlayerNames::SendJoinName / OnJoinText). The rule itself lives
// in AccessRules.hpp. Keys are never printed or logged; names are.
//
// The list: -rbusers=<path> (tests) or <Win64>\reborn-users.json. No file: anyone can join. The
// file is re-read when its last-write time changes (checked every 2 s). Unreadable at the first
// read: every join is refused (a PROBLEM line). Unreadable later: the last good list stays (logged
// once). After each successful re-read every joined player is checked again.
//
// A refused player gets "RebornRefused=Improper Credentials!" (NMT_DebugText) and its connection
// is closed by the engine's own close 1 s later, so the message goes out first.
namespace Access {
    // Server, every engine tick: the first call reads the list and logs it; then re-reads, the
    // re-check of joined players, and the delayed closes of refused players.
    void Tick(float dt);

    // Server, on NMT_Join: true lets the player in (logs "[ACCESS] "<name>" may join ...");
    // false refuses it (logs "[ACCESS] refused "<name>": <reason>", tells the player, closes it).
    bool AllowJoin(Globals::ServerPlayer& sp);

    // Server: whether this player passes the current list (no logging, no side effects).
    bool CanJoinNow(const Globals::ServerPlayer& sp);

    // Client, on NMT_DebugText (joining and in the map): "RebornRefused=<text>" is the server
    // turning us away. Keeps the text (made safe, at most 80 characters) and logs
    // "[ACCESS] client: refused by the server: <text>".
    void OnClientText(void* inbunch);

    // Client: the last refusal the server sent (for the screen that tells the player).
    bool HasRefusal();
    std::string Refusal();
    void ClearRefusal();
}

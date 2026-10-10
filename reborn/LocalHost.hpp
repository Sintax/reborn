#pragma once
#include <string>

// "Host Game" from the in-game menu: start a dedicated server on this PC, wait until it is ready,
// then connect the host's own game to it. Friends join with Direct Connect to the host's address.
//
// The server is Serverborn.exe (a copy of Battleborn.exe) started through its own copy of the Steam
// loader in Win64\rb_ids\host-server, the same layout debugloop/host.py uses. It runs in a job
// object, so it closes when the host's game closes or when the host starts another game.
namespace LocalHost {
    enum class Status { Idle, Starting, ReadyToJoin, Running, Failed };

    // mapUrl is what the server opens, e.g. "Caverns_P". Returns at once; the work runs on its own thread.
    void Start(const std::string& mapUrl, const std::string& displayName, int players);
    // Stops a server that is starting or running.
    void Stop();

    Status GetStatus();
    std::string GetStatusText();
    // True once, when the server has just become ready (the caller then connects to it).
    bool TakeReadyToJoin();
    // Clears a failure message after the player has read it.
    void Dismiss();
}

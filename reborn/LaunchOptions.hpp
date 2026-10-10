#pragma once
#include <string>
#include <vector>

namespace LaunchOptions {
    struct Options {
        std::wstring instance = L"game";
        int debugPort = 0;            // 0 = no debug server
        std::wstring runDir;          // empty = Documents\RebornLogs\<instance>-<time>
        bool autopilot = false;
        bool combat = false;          // client: autopilot fights (Combat.cpp) instead of wandering
        std::wstring join;            // client: "IP:PORT" to connect to
        std::wstring host;            // client: press "Host Game" for this map at the menu (LocalHost.cpp)
        std::wstring soloMap;         // solo: map to open
        std::wstring serverMap;       // server: travel URL, e.g. "Dojo_P" or "IceScort_P?SpawnBotsTeamA=4"
        int players = 0;              // server: players to wait for (0 = keep default)
        std::wstring readyEvent;      // server: named event to set once it is listening (LocalHost.cpp)
        std::string coordinator;      // "host:port" of the matchmaking service (empty = Constants::GameCoordinatorEndpoint)
        std::string coordinatorKey;   // server: the token the service gave this server; with coordinator, turns on coordinator mode
        std::string character;        // display name from Constants::CharacterSelectCharacterTable
        unsigned int seed = 0;
        int hangSeconds = 60;
        bool testHang = false;        // freeze the game thread 20 s after play starts
        bool testCrash = false;       // null write 20 s after play starts
        std::vector<std::wstring> errors;
        std::vector<std::wstring> unknown;
    };

    Options Parse(const std::wstring& commandLine);
    // The -rbinstance value straight from the raw command line, or "" if absent.
    // Safe in DllMain: no shell32, no allocation beyond the returned string.
    std::wstring InstanceArg(const std::wstring& commandLine);
    void InitFromCommandLine();
    const Options& Get();
    // What the game's single-instance mutex check got (dllmain.cpp), for startup-failure logs.
    std::string SingleInstanceMutexStatus();
}

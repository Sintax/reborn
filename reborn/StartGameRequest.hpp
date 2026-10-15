#pragma once
#include <string>
#include "json.hpp"

// What the "Start a game on the server" panel sends to the matchmaking service
// (gamecontroller GameRequest.cs). Pure data so it can be unit tested.
namespace StartGame {
    struct Request {
        std::string name;        // shown in the server browser
        std::string mapMode;     // "Story - The Algorithm"
        std::string mapCommand;  // "open Caverns_P"
        std::string playerName;
        int players = 1;
        int botsTeamA = 5;
        int botsTeamB = 5;
    };

    inline bool IsVersus(const std::string& mapMode) {
        return !(mapMode.starts_with("Story") || mapMode.starts_with("Operations"));
    }

    inline int MaxPlayersFor(const std::string& mapMode) { return IsVersus(mapMode) ? 10 : 5; }

    inline std::string ToJson(const Request& r) {
        nlohmann::json j;
        j["Name"] = r.name;
        j["MapMode"] = r.mapMode;
        j["MapCommand"] = r.mapCommand;
        j["Players"] = r.players;
        j["BotsTeamA"] = r.botsTeamA;
        j["BotsTeamB"] = r.botsTeamB;
        j["PlayerName"] = r.playerName;
        return j.dump();
    }
}

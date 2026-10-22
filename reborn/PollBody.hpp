#pragma once

#include "json.hpp"

#include <string>
#include <vector>

// The body of the game server's 5 s poll to matchmaking (POST /api/games/server-poll), kept free
// of the SDK so the unit tests can build it. It carries the two old fields plus who is in the
// game. Never put a player's Key here.

struct PollPlayer {
    std::string name;   // the cleaned, unique join name
    std::string hero;   // display name ("Oscar Mike"); empty until a hero is picked
    int team = -1;      // team index; -1 when unknown
};

// At most this many players are listed; the rest are dropped (ConnectedPlayers still counts all).
constexpr size_t kPollMaxPlayers = 32;

// {"ConnectedPlayers":n,"HumansHaveStarted":b,"Players":[{"Name":..,"Hero":..,"Team":..},...]}
// Built with nlohmann::json, so odd characters in names are escaped.
inline std::string MakePollBody(int connectedPlayers, bool humansHaveStarted, const std::vector<PollPlayer>& players) {
    nlohmann::json body = nlohmann::json::object();
    body["ConnectedPlayers"] = connectedPlayers;
    body["HumansHaveStarted"] = humansHaveStarted;
    nlohmann::json list = nlohmann::json::array();
    for (const PollPlayer& p : players) {
        if (list.size() >= kPollMaxPlayers) break;
        list.push_back({ { "Name", p.name }, { "Hero", p.hero }, { "Team", p.team } });
    }
    body["Players"] = std::move(list);
    // replace: a name that is not valid UTF-8 must not make dump() throw on the game thread.
    return body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

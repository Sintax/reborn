#pragma once

#include <string>
#include <optional>

#include "json.hpp"
#include "httplib.h"
#include "StartGameRequest.hpp"

namespace GameCoordinator {
    struct ServerBrowserEntry {
        std::string InstanceName = "";
        std::string HumanReadableInstanceMapMode = "";
        int CurrentNumPlayers = 0;
        int MaxNumPlayers = 0;
        std::string ServerConnectString = "";
        bool MatchStarted = false;
        // Set for games started through the matchmaking service; older servers omit these.
        std::string MyGuid = "";
        bool Ready = false;
        bool AllowLateJoin = false;

        ServerBrowserEntry(const nlohmann::json& inJson) {
            InstanceName = inJson["instanceName"];
            HumanReadableInstanceMapMode = inJson["humanReadableInstanceMapMode"];
            CurrentNumPlayers = inJson["currentNumPlayers"];
            MaxNumPlayers = inJson["maxNumPlayers"];
            ServerConnectString = inJson["serverConnectString"];
            MatchStarted = inJson["matchStarted"];
            MyGuid = inJson.value("myGuid", std::string());
            Ready = inJson.value("ready", false);
            AllowLateJoin = inJson.value("allowLateJoin", false);
        }

        ServerBrowserEntry() {

        }
    };

    // "host:port" of the matchmaking service: -rbcoordinator, else the built-in default.
    std::string Endpoint();
    // The client used for every call to the service, made on first use.
    httplib::Client& Client();

    void RefreshServerBrowser();

    struct StartResult { bool ok = false; std::string guid, connectString, error; };
    // POST /api/games/start with the player's key file. Blocking: call on a thread.
    StartResult StartGame(const StartGame::Request& request);
    // DELETE /api/games/{guid}: shuts down a game this player started.
    bool CancelGame(const std::string& guid);
    // A copy of the browser entry for a started game, or nullopt until it is listed.
    // A copy, because the browser list is refilled on another thread.
    std::optional<ServerBrowserEntry> FindEntry(const std::string& guid);
    // A copy of the whole browser list, taken under the lock. Use this instead of reading Globals::ServerBrowserEntries.
    std::vector<ServerBrowserEntry> BrowserEntriesSnapshot();
}

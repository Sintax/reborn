#pragma once
#include <string>
#include <utility>

#include "Constants.hpp"
#include "StartGameRequest.hpp"

// What the player has chosen in the "Start a Game" window. Pure data so it can be unit tested.
struct StartGameModel {
    bool runOnServer = false;   // true: the matchmaking server starts it; false: this PC hosts it
    bool hasKey = false;        // reborn-key.txt is present (needed to start games on the server)
    int mapIndex = 1;           // into Constants::GameCreationOptions ("Story - The Algorithm")
    int players = 1;            // players to wait for, including you
    int botsA = 5, botsB = 5;   // Versus only
    int draftStyleIndex = 0;    // into Constants::DraftStyleNames; Versus only
    std::string gameName;       // server only: shown in the server browser

    bool ServerChoiceEnabled() const { return hasKey; }

    const std::pair<std::string, std::string>& MapMode() const {
        const auto& all = Constants::GameCreationOptions;
        return all[mapIndex >= 0 && mapIndex < (int)all.size() ? mapIndex : 0];
    }

    bool IsVersus() const { return StartGame::IsVersus(MapMode().first); }
    int MaxPlayers() const { return StartGame::MaxPlayersFor(MapMode().first); }

    // The name sent to the matchmaking service ("BanPick").
    const std::string& DraftStyleName() const {
        const auto& all = Constants::DraftStyleNames;
        return all[draftStyleIndex >= 0 && draftStyleIndex < (int)all.size() ? draftStyleIndex : 0].second;
    }

    // My PC: the map URL the local server opens, with the bots for Versus (as the server adds them).
    std::string HostMapUrl() const {
        const std::string& cmd = MapMode().second;
        std::string url = cmd.starts_with("open ") ? cmd.substr(5) : cmd;
        if (IsVersus()) url += "?SpawnBotsTeamA=" + std::to_string(botsA) + "?SpawnBotsTeamB=" + std::to_string(botsB);
        return url;
    }

    // My PC: the game's draft style value for ?DraftStyle=; empty for Story and for Quick Select
    // (the game's default).
    std::string HostDraftStyleValue() const {
        if (!IsVersus() || DraftStyleName() == "QuickSelect") return "";
        return StartGame::DraftStyleValue(DraftStyleName(), StartGame::TeamSize(HostMapUrl(), players));
    }
};

namespace StartGameWindow {
    // Draws the window when it is open or a start is in progress, and every frame follows a start
    // that is under way (joins the game when it is ready). Call once per frame.
    void Draw(StartGameModel& m);
}

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
        std::string draftStyle = "QuickSelect";   // a Constants::DraftStyleNames name; Versus only
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
        j["draftStyle"] = r.draftStyle;
        return j.dump();
    }

    // The game's ?DraftStyle= value for a draft style name: the style's CodeName, as the server's
    // "[CHARSEL] style ... code" dump prints it (no underscore: "BanPick3v3", not the object name
    // CharacterSelectStyle_PvP_BanPick_3v3). teamSize is players per side, 3 or fewer uses the
    // 3v3 variant. "" for an unknown name.
    inline std::string DraftStyleValue(const std::string& name, int teamSize) {
        std::string size = teamSize <= 3 ? "3v3" : "5v5";
        if (name == "QuickSelect" || name == "ChaosRumble" || name == "Classic") return name;
        if (name == "BanPick") return "BanPick" + size;
        if (name == "CaptainsDraft") return "Captains" + size;
        return "";
    }

    // The number after "?<option>=" in a map URL, or 0.
    inline int UrlNumber(const std::string& url, const std::string& option) {
        size_t at = url.find("?" + option + "=");
        if (at == std::string::npos) return 0;
        try { return std::stoi(url.substr(at + option.size() + 2)); }
        catch (...) { return 0; }
    }

    // Players per side for a hosted map URL: the people plus its ?SpawnBotsTeamA/B= bots, split over
    // two teams, rounded up (the coordinator's DraftStyles.TeamSize).
    inline int TeamSize(const std::string& mapUrl, int players) {
        return (players + UrlNumber(mapUrl, "SpawnBotsTeamA") + UrlNumber(mapUrl, "SpawnBotsTeamB") + 1) / 2;
    }

    // The server's map URL with ?DraftStyle=<value> added; unchanged when value is empty.
    inline std::string WithDraftStyle(const std::string& mapUrl, const std::string& value) {
        return value.empty() ? mapUrl : mapUrl + "?DraftStyle=" + value;
    }
}

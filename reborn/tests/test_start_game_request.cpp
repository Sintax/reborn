// reborn/tests/test_start_game_request.cpp
#include "../StartGameRequest.hpp"
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

int main() {
    CHECK(!StartGame::IsVersus("Story - The Algorithm"));
    CHECK(!StartGame::IsVersus("Operations - Toby's Friendship Raid"));
    CHECK(StartGame::IsVersus("Incursion - Overgrowth"));
    CHECK(StartGame::MaxPlayersFor("Story - Sentinel") == 5);
    CHECK(StartGame::MaxPlayersFor("Meltdown - Paradise") == 10);

    StartGame::Request r;
    r.name = "DJ's \"Overgrowth\"";
    r.mapMode = "Incursion - Overgrowth";
    r.mapCommand = "open Inc_Stronghold2_P";
    r.players = 2; r.botsTeamA = 5; r.botsTeamB = 3; r.playerName = "DJ";
    std::string j = StartGame::ToJson(r);
    CHECK(j.find("\"Name\":\"DJ's \\\"Overgrowth\\\"\"") != std::string::npos);
    CHECK(j.find("\"MapMode\":\"Incursion - Overgrowth\"") != std::string::npos);
    CHECK(j.find("\"MapCommand\":\"open Inc_Stronghold2_P\"") != std::string::npos);
    CHECK(j.find("\"Players\":2") != std::string::npos);
    CHECK(j.find("\"BotsTeamA\":5") != std::string::npos);
    CHECK(j.find("\"BotsTeamB\":3") != std::string::npos);
    CHECK(j.find("\"PlayerName\":\"DJ\"") != std::string::npos);
    CHECK(j.find("\"draftStyle\":\"QuickSelect\"") != std::string::npos);
    r.draftStyle = "BanPick";
    CHECK(StartGame::ToJson(r).find("\"draftStyle\":\"BanPick\"") != std::string::npos);

    // Draft styles: the game's CodeNames (server log "[CHARSEL] style ... code").
    CHECK(StartGame::DraftStyleValue("BanPick", 5) == "BanPick5v5");
    CHECK(StartGame::DraftStyleValue("BanPick", 3) == "BanPick3v3");
    CHECK(StartGame::DraftStyleValue("CaptainsDraft", 4) == "Captains5v5");
    CHECK(StartGame::DraftStyleValue("CaptainsDraft", 2) == "Captains3v3");
    CHECK(StartGame::DraftStyleValue("QuickSelect", 5) == "QuickSelect");
    CHECK(StartGame::DraftStyleValue("ChaosRumble", 3) == "ChaosRumble");
    CHECK(StartGame::DraftStyleValue("Classic", 5) == "Classic");
    CHECK(StartGame::DraftStyleValue("Turbo", 5).empty());
    CHECK(StartGame::DraftStyleValue("banpick", 5).empty());

    // Hosted map URLs: team size from the people plus the URL's bots; ?DraftStyle= added when set.
    CHECK(StartGame::TeamSize("Viaduct_P?SpawnBotsTeamA=2?SpawnBotsTeamB=3", 1) == 3);
    CHECK(StartGame::TeamSize("Viaduct_P?SpawnBotsTeamA=4?SpawnBotsTeamB=5", 1) == 5);
    CHECK(StartGame::TeamSize("Caverns_P", 2) == 1);
    CHECK(StartGame::WithDraftStyle("Viaduct_P?SpawnBotsTeamA=2", "BanPick3v3") == "Viaduct_P?SpawnBotsTeamA=2?DraftStyle=BanPick3v3");
    CHECK(StartGame::WithDraftStyle("Viaduct_P", "") == "Viaduct_P");

    std::printf(failures ? "FAILED %d\n" : "OK\n", failures);
    return failures ? 1 : 0;
}

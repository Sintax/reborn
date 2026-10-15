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

    std::printf(failures ? "FAILED %d\n" : "OK\n", failures);
    return failures ? 1 : 0;
}

// reborn/tests/test_poll_body.cpp
#include "../PollBody.hpp"
#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

using nlohmann::json;

static void empty_list_keeps_the_old_fields() {
    json j = json::parse(MakePollBody(3, true, {}));
    CHECK(j["ConnectedPlayers"] == 3);
    CHECK(j["HumansHaveStarted"] == true);
    CHECK(j["Players"].is_array() && j["Players"].empty());
    // Exact text: the old two fields, in this order, then the list.
    CHECK(MakePollBody(0, false, {}) == R"({"ConnectedPlayers":0,"HumansHaveStarted":false,"Players":[]})");
}

static void players_are_listed_in_order() {
    std::vector<PollPlayer> ps = { {"dj", "Oscar Mike", 0}, {"Kaz", "Rath", 1} };
    json j = json::parse(MakePollBody(2, false, ps));
    CHECK(j["Players"].size() == 2);
    CHECK(j["Players"][0]["Name"] == "dj");
    CHECK(j["Players"][0]["Hero"] == "Oscar Mike");
    CHECK(j["Players"][0]["Team"] == 0);
    CHECK(j["Players"][1]["Name"] == "Kaz");
    CHECK(j["Players"][1]["Hero"] == "Rath");
    CHECK(j["Players"][1]["Team"] == 1);
}

static void names_with_quotes_are_escaped() {
    std::vector<PollPlayer> ps = { {"a\"b", "x\\y", 0} };
    std::string body = MakePollBody(1, false, ps);
    json j = json::parse(body);   // throws if the text is not valid JSON
    CHECK(j["Players"][0]["Name"] == "a\"b");
    CHECK(j["Players"][0]["Hero"] == "x\\y");
}

static void hero_empty_and_team_minus_one_are_kept() {
    std::vector<PollPlayer> ps = { {"newbie", "", -1} };
    json j = json::parse(MakePollBody(1, false, ps));
    CHECK(j["Players"][0]["Hero"] == "");
    CHECK(j["Players"][0]["Team"] == -1);
}

static void more_than_32_players_are_capped() {
    std::vector<PollPlayer> ps;
    for (int i = 0; i < 40; i++) ps.push_back({ "p" + std::to_string(i), "", 0 });
    json j = json::parse(MakePollBody(40, false, ps));
    CHECK(j["Players"].size() == 32);
    CHECK(j["Players"][31]["Name"] == "p31");   // the first 32 stay, in order
    CHECK(j["ConnectedPlayers"] == 40);          // the count is not capped
}

int main() {
    empty_list_keeps_the_old_fields();
    players_are_listed_in_order();
    names_with_quotes_are_escaped();
    hero_empty_and_team_minus_one_are_kept();
    more_than_32_players_are_capped();
    if (failures) { std::printf("%d check(s) failed\n", failures); return 1; }
    std::printf("test_poll_body: all passed\n");
    return 0;
}

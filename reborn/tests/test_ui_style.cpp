// reborn/tests/test_ui_style.cpp
#include "../UiStyle.hpp"
#include "../StartGameWindow.hpp"
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static int IndexOf(const std::string& mapMode) {
    for (int i = 0; i < (int)Constants::GameCreationOptions.size(); i++)
        if (Constants::GameCreationOptions[i].first == mapMode) return i;
    return -1;
}

int main() {
    // Sizes: windows 40 % of the screen width, buttons 4 % of its height kept within 28..56 px.
    CHECK(UiStyle::WindowWidth(2560.f) == 1024.f);
    CHECK(UiStyle::ButtonHeight(1440.f) == 56.f);   // 4 % = 57.6 -> clamped
    CHECK(UiStyle::ButtonHeight(720.f) == 28.8f);
    CHECK(UiStyle::ButtonHeight(360.f) == 28.f);    // a 640x360 test window: clamped up
    CHECK(UiStyle::FontScale(1080.f) == 2.f);
    CHECK(UiStyle::FontScale(360.f) == 1.f);

    // "Run on: the server" needs the server key.
    StartGameModel m; m.hasKey = false; m.runOnServer = true;
    CHECK(!m.ServerChoiceEnabled());                 // review-focus 5
    m.hasKey = true; CHECK(m.ServerChoiceEnabled());

    // Versus shows bots and draft style; Story does not.
    m.mapIndex = IndexOf("Story - The Algorithm");
    CHECK(m.mapIndex >= 0);
    CHECK(!m.IsVersus());
    CHECK(m.MaxPlayers() == 5);
    CHECK(m.HostMapUrl() == "Caverns_P");
    CHECK(m.HostDraftStyleValue().empty());
    m.mapIndex = IndexOf("Incursion - Overgrowth");
    CHECK(m.mapIndex >= 0);
    CHECK(m.IsVersus());
    CHECK(m.MaxPlayers() == 10);

    // My PC: bots go on the map URL; Ban Pick picks 3v3 or 5v5 from people plus bots per side.
    m.players = 1; m.botsA = 2; m.botsB = 2;
    m.draftStyleIndex = 1;   // Ban Pick
    CHECK(m.DraftStyleName() == "BanPick");
    CHECK(m.HostMapUrl().ends_with("?SpawnBotsTeamA=2?SpawnBotsTeamB=2"));
    CHECK(m.HostDraftStyleValue() == "BanPick3v3");  // (1 + 2 + 2) / 2 rounded up = 3
    m.botsA = 5; m.botsB = 5;
    CHECK(m.HostDraftStyleValue() == "BanPick5v5");
    m.draftStyleIndex = 0;   // Quick Select: the game's default, nothing added
    CHECK(m.HostDraftStyleValue().empty());

    // A broken index never reads past the lists.
    m.mapIndex = 999; m.draftStyleIndex = -3;
    CHECK(m.MapMode().first == Constants::GameCreationOptions[0].first);
    CHECK(m.DraftStyleName() == "QuickSelect");

    if (failures == 0) std::printf("PASS\n");
    return failures == 0 ? 0 : 1;
}

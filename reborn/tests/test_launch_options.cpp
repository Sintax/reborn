#include "../LaunchOptions.hpp"
#include "../CharSelectRules.hpp"
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

int main() {
    auto o = LaunchOptions::Parse(LR"(Battleborn.exe -windowed -rbinstance=c1 -rbdebugport=18081 -rbautopilot "-rbcharacter=Oscar Mike" -rbjoin=127.0.0.1:7777 -rbseed=7 -rbhangsecs=15 -rbtesthang)");
    CHECK(o.instance == L"c1");
    CHECK(o.debugPort == 18081);
    CHECK(o.autopilot);
    CHECK(o.character == "Oscar Mike");
    CHECK(o.join == L"127.0.0.1:7777");
    CHECK(o.seed == 7);
    CHECK(o.hangSeconds == 15);
    CHECK(o.testHang && !o.testCrash);
    CHECK(o.errors.empty() && o.unknown.empty());

    auto s = LaunchOptions::Parse(LR"(Serverborn.exe -rbservermap=IceScort_P?SpawnBotsTeamA=4 -rbplayers=2)");
    CHECK(s.serverMap == L"IceScort_P?SpawnBotsTeamA=4");
    CHECK(s.players == 2);
    CHECK(s.name.empty());
    CHECK(LaunchOptions::Parse(L"Battleborn.exe -rbname=Test-c2").name == "Test-c2");

    // Matchmaking: the client points at the coordinator; a coordinator-launched server also gets its key.
    auto c = LaunchOptions::Parse(L"Battleborn.exe -rbcoordinator=203.0.113.10:5000");
    CHECK(c.coordinator == "203.0.113.10:5000");
    CHECK(c.coordinatorKey.empty());
    CHECK(!c.browse);
    CHECK(LaunchOptions::Parse(L"Battleborn.exe -rbautopilot -rbbrowse").browse);
    auto g = LaunchOptions::Parse(L"Serverborn.exe -rbinstance=rb-algo -rbcoordinator=localhost:5000 -rbcoordinatorkey=abc-123");
    CHECK(g.coordinator == "localhost:5000");
    CHECK(g.coordinatorKey == "abc-123");
    CHECK(g.errors.empty() && g.unknown.empty());

    auto d = LaunchOptions::Parse(L"Battleborn.exe");
    CHECK(d.instance == L"game" && d.debugPort == 0 && d.hangSeconds == 60 && !d.autopilot);

    auto bad = LaunchOptions::Parse(L"Battleborn.exe -rbdebugport=80 -rbhangsecs=abc -rbwhat");
    CHECK(bad.errors.size() == 2);
    CHECK(bad.unknown.size() == 1);
    CHECK(bad.debugPort == 0 && bad.hangSeconds == 60);

    // InstanceArg runs in DllMain, so it reads the raw command line itself (no shell32).
    CHECK(LaunchOptions::InstanceArg(L"Battleborn.exe -windowed -rbinstance=c1 -rbdebugport=18081") == L"c1");
    CHECK(LaunchOptions::InstanceArg(L"Battleborn.exe -rbinstance=server") == L"server");
    CHECK(LaunchOptions::InstanceArg(LR"(Battleborn.exe "-rbinstance=my box" -x)") == L"my box");
    CHECK(LaunchOptions::InstanceArg(L"Battleborn.exe -windowed").empty());
    CHECK(LaunchOptions::InstanceArg(L"Battleborn.exe -rbinstance= -x").empty());

    auto sg = LaunchOptions::Parse(LR"(Battleborn.exe -rbautopilot "-rbstartgame=Incursion - Overgrowth" -rbstartplayers=2 -rbbots=5,3)");
    CHECK(sg.startGame == "Incursion - Overgrowth");
    CHECK(sg.startPlayers == 2);
    CHECK(sg.botsA == 5 && sg.botsB == 3);
    auto sgDefaults = LaunchOptions::Parse(L"Battleborn.exe -rbautopilot \"-rbstartgame=Story - The Algorithm\"");
    CHECK(sgDefaults.startPlayers == 1 && sgDefaults.botsA == 5 && sgDefaults.botsB == 5);
    CHECK(!sgDefaults.randomHero);
    CHECK(LaunchOptions::Parse(L"Serverborn.exe -rbrandomhero -rbinstance=x").randomHero);
    CHECK(!sgDefaults.minConsole);
    CHECK(LaunchOptions::Parse(L"Battleborn.exe -rbminconsole").minConsole);
    CHECK(!LaunchOptions::Parse(L"Battleborn.exe -rbbots=9,1").errors.empty());
    CHECK(sgDefaults.draftStyle == "QuickSelect");
    auto ds = LaunchOptions::Parse(L"Battleborn.exe -rbautopilot -rbhost=Viaduct_P -rbdraftstyle=BanPick");
    CHECK(ds.draftStyle == "BanPick" && ds.errors.empty() && ds.unknown.empty());

    // Every networked game runs the game's own character select: the server's map string asks for it once.
    CHECK(CharSelect::WithTournamentMode(L"open Caverns_P") == L"open Caverns_P?bTournamentMode=1");
    CHECK(CharSelect::WithTournamentMode(L"open Viaduct_P?SpawnBotsTeamA=4?SpawnBotsTeamB=5") == L"open Viaduct_P?SpawnBotsTeamA=4?SpawnBotsTeamB=5?bTournamentMode=1");
    CHECK(CharSelect::WithTournamentMode(L"open Caverns_P?bTournamentMode=1") == L"open Caverns_P?bTournamentMode=1");

    std::puts(failures ? "FAIL" : "PASS");
    return failures;
}

#include "../LaunchOptions.hpp"
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

    std::puts(failures ? "FAIL" : "PASS");
    return failures;
}

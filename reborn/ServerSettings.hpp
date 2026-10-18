#pragma once

#include <string>

namespace ServerSettings {
    extern int gamePort;

    extern float tickrate;

    extern unsigned int NumPlayersToStart;

    extern unsigned int TeamMinSizeForStart;

    extern const wchar_t* MapString;

    extern bool amRunningWithGameCoordinator;

    extern std::string GameCoordinatorBase;

    extern std::string GameCoordinatorKey;

    // Every player gets the hero the game's pick timer chooses (the coordinator's "randomHero",
    // or the -rbrandomhero test switch).
    extern bool RandomHero;
}
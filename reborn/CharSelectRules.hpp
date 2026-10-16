#pragma once

#include <string>

// The string-only part of CharSelect, kept free of the SDK so the unit tests can build it.
namespace CharSelect {
    // The server's "open <map>?..." command with ?bTournamentMode=1 added once. Tournament mode is
    // what makes the game run its own character-select phase for a networked match.
    inline std::wstring WithTournamentMode(const std::wstring& openCommand) {
        static const std::wstring option = L"?bTournamentMode=1";
        if (openCommand.find(option) != std::wstring::npos) return openCommand;
        return openCommand + option;
    }
}

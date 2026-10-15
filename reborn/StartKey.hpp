// reborn/StartKey.hpp
#pragma once
#include <filesystem>
#include <string>

// The per-friend start key (Win64\reborn-key.txt) that lets a player start games on the rented
// server, and the player's display name (Win64\reborn-player.txt). Both are plain one-line files.
namespace StartKey {
    std::string TrimKey(const std::string& fileText);
    std::string DefaultGameName(const std::string& player, const std::string& mapMode);

    std::filesystem::path Win64Dir();
    std::string Key();
    std::string PlayerName();
    void SavePlayerName(const std::string& name);
}

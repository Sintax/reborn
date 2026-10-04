#pragma once
#include <string>

namespace GameState {
    std::string SnapshotJson();
    std::string Exec(const std::string& command);
    void SetListening(bool listening);
}

#pragma once
#include <string>

class UObject;

namespace GameState {
    std::string SnapshotJson();
    std::string Exec(const std::string& command);
    void SetListening(bool listening);

    // A hero definition (a player's picked character, or a pawn's class definition) as its display
    // name ("Oscar Mike"). "" for null, a destroyed object, or a definition that names no known hero.
    // Takes UObject* so both UPoplarPlayerNameIdentifierDefinition* and the pawn's class def fit.
    std::string HeroDisplayName(UObject* def);
}

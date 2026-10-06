#pragma once
#include "BB/SdkHeaders.hpp"
#include <string>

// Pawn checks shared by GameState (the /state snapshot) and Combat (the enemy census).
namespace PawnUtils {
    constexpr uint64_t kPendingKill = 0x2000000000000000ull;

    inline bool Gone(UObject* o) { return !o || (o->ObjectFlags & kPendingKill); }

    inline bool IsDefault(UObject* o) { return o->GetFullName().find("Default__") != std::string::npos; }

    // A live pawn in a level (not an archetype or class default, not being destroyed).
    inline bool LivePawnInWorld(APawn* p) {
        return !Gone(p) && !p->bDeleteMe && !p->bTearOff && p->Outer && p->Outer->IsA(ULevel::StaticClass());
    }

    inline std::string HeroOf(APawn* p) {
        if (p->IsA(APoplarPawn::StaticClass())) {
            UPoplarPlayerClassDefinition* def = reinterpret_cast<APoplarPawn*>(p)->PoplarPlayerClassDef;
            if (!Gone(def)) return def->GetName();
        }
        return !Gone(p->ObjectArchetype) ? p->ObjectArchetype->GetName() : std::string("unknown");
    }

    // "player" = a person, "bot" = a hero played by the AI, "minion" = everything else.
    inline std::string KindOf(APawn* p) {
        APlayerReplicationInfo* pri = p->PlayerReplicationInfo;
        if (Gone(pri)) return "minion";
        return pri->bBot ? "bot" : "player";
    }
}

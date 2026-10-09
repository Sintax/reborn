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

    // A minion's health as its ResourcePoolManager replicates it: HealthPool names the manager
    // and the pool's slot there (ReplicatedValueStates is the replicated copy of each pool's value).
    // -1 when the manager or the slot is not known on this machine.
    inline float ReplicatedPoolHealth(APawn* p) {
        AResourcePoolManager* m = p->HealthPool.PoolManager;
        uint8_t i = p->HealthPool.PoolIndexInManager;
        if (Gone(m) || i >= 16) return -1.f;
        if (m->ReplicatedPoolIdentities[i].PoolGUID != p->HealthPool.PoolGUID) return -1.f;
        return m->ReplicatedValueStates[i].CurrentValue;
    }

    // A non-player pawn's health on this machine, -1 when unknown but alive. On a networked client
    // GetHealth reads 0 for a live minion (it has no PRI to replicate its pool through), so the
    // pool manager's replicated value is used; if that is unknown too, the pawn counts as alive
    // unless it has gone ragdoll (live runs 20261007-071315, 20261009-041344).
    inline float MinionHealth(APawn* p, bool client) {
        float h = p->GetHealth();
        if (h > 0.f || !client || !Gone(p->PlayerReplicationInfo)) return h;
        float pool = ReplicatedPoolHealth(p);
        if (pool > 0.f) return pool;
        if (pool < 0.f && p->Physics != EPhysics::PHYS_RigidBody) return -1.f;
        return 0.f;
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

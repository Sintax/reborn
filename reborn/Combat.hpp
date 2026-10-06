#pragma once
#include "BB/SdkHeaders.hpp"
#include "json.hpp"
#include <string>

// The autopilot's combat brain: enemy census, target choice, aim, fire, skills, movement intent.
// Client only. Runs on the game thread from Autopilot::PlayTick.
namespace Combat {
    struct Enemy {
        uintptr_t id;          // the pawn pointer, the stable id the tools use
        std::string hero;
        std::string kind;      // player | bot | minion
        float loc[3];
        float distance;
        float bearingDeg;      // -180..180 relative to the view yaw; positive = to the right
        float pitchDeg;        // positive = above the eye line
        bool visible;
        float health;
        int team;
    };

    bool Enabled();
    void SetEnabled(bool on);
    void Tick(float dt, APoplarPlayerController* pc);
    void OnNoPawn();
    std::string SnapshotJson();
    nlohmann::json StatsJson();
}

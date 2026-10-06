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

    // Movement/look input for this frame. The autopilot writes it when valid; otherwise it wanders.
    struct Axes { float forward = 1.f, strafe = 0.f, turn = 0.f, lookUp = 0.f; bool valid = false; };

    struct Tuning {
        float fireRange = 3000.f;     // fire only inside this (units; ~30 m)
        float engageRange = 1200.f;   // hunt: approach until this close
        float aimOnDeg = 4.f;         // aim counts as on under this yaw and pitch error
        float maxTurnDegPerTick = 20.f;
        float retreatHealthFrac = 0.25f;
        float retreatNearUnits = 800.f;
        float playerWeight = 0.6f;    // players score closer than bots/minions
        float targetMemoryS = 3.f;    // drop a target unseen this long
    };

    // The standing order. mode: hunt | hold | goto | follow | retreat | wander.
    struct Order {
        std::string mode = "hunt";
        uintptr_t target = 0;
        bool hasPoint = false;
        float point[3] = { 0, 0, 0 };
        bool fire = true;
        bool skills = true;
    };

    bool Enabled();
    void SetEnabled(bool on);
    void Tick(float dt, APoplarPlayerController* pc);
    void OnNoPawn();
    Axes CurrentAxes();
    Tuning& GetTuning();
    const Order& CurrentOrder();
    std::string SetOrder(const Order& o);   // "" or an error word (unknown_mode)
    const char* ModeName();

    std::string SnapshotJson();                          // GET /combat
    nlohmann::json StatsJson();                          // the "combat" stats in /state
    std::string ApplyOrderJson(const std::string& body); // POST /order
    std::string ActJson(const std::string& body);        // POST /act
}

#pragma once

namespace Autopilot {
    bool Active();
    void OnMainMenuReady();
    // Startup finished and the save picker would open. Returns true when the autopilot takes over
    // (loads or creates a save and continues to the menu on its next tick) instead of the picker.
    bool OnStartupComplete();
    void Tick(float dt);
    const char* PhaseName();
    // The game's pick phase in this match is a timer-only pick (a RandomHero game: the server set
    // it to 5 s), so the autopilot does not click and the timer picks the hero.
    bool PickLockedByServer();
}

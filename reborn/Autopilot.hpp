#pragma once

class UObject;

namespace Autopilot {
    bool Active();
    void OnMainMenuReady();
    // Startup finished and the save picker would open. Returns true when the autopilot takes over
    // (loads or creates a save and continues to the menu on its next tick) instead of the picker.
    bool OnStartupComplete();
    void Tick(float dt);
    void BeforePlayerTick(UObject* controller);
    const char* PhaseName();
}

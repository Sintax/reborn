#pragma once

class UObject;

namespace Autopilot {
    bool Active();
    void OnMainMenuReady();
    void Tick(float dt);
    void BeforePlayerTick(UObject* controller);
    const char* PhaseName();
}

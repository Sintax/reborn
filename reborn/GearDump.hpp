#pragma once

namespace GearDump {
    // With -rbdumpgear: once the gear definitions are loaded, writes every PoplarPerkFunction's
    // stat data (card text, presentations, skill effects, activation cost, and the formulas they
    // use, raw and evaluated) to <run dir>\gear-stats.json. Logs "[GEARDUMP] ...".
    void Tick(float dt);
}

#pragma once

// The main menu's Command screen -> Gear tab, offline. The game fills that tab from its online
// item service (Hydra), which is gone: every request failed with "an invalid service has been
// specified" and the tab stayed empty. So the client's request sender is hooked
// (battleborn+0x15beaa0) and the Command screen's requests are answered locally:
//  - the gear list (sideboards) and the loadouts (meta_profiles) get replies built from the
//    save (CommandGearRules.hpp), parsed by the game's own parsers into its item store;
//  - entitlements, item unlock schedule, marketplace and stats just report success;
//  - saving a loadout on the Gear tab writes the gear set into the save.
// Logs start with "[GEARTAB]".
namespace CommandGear {
    // Client only, at startup.
    void InstallHooks();
}

#pragma once

#include "GearSets.hpp"

class APoplarPlayerController;

// Gear sets in the game's own hero screen (the loadout row under the hero grid). The rules and
// the message format are in GearSets.hpp; this is the part that touches game objects.
//
// How the game uses loadouts (Ghidra):
//  - The hero screen copies the first NumLoadoutEntitlements of the player's meta PRI
//    ReplicatedLoadouts once, when its "received" check (battleborn+0x112f580) first passes
//    (+0x11560d0, then "GotDemLoadouts" to the movie), and selects cell 0.
//  - A pick sends PoplarPlayerController.ServerSelectPlayerMetaLoadoutIndex(i); the server copies
//    ReplicatedLoadouts[i] into CurrentLoadout (+0x1130510).
// So the server holds NumLoadoutEntitlements at -1 (check not passed, the screen waits) until the
// client's sets arrive, then sets it to the number of sets it filled. Clients receive that count
// and the sets' names, but not their perk objects, so each client then writes the same sets from
// its own save into its own copy (ClientTick) and the screen shows them.
namespace GearSets {
    // Server, at login (CharSelect::GrantEverything): hold the player's loadout count at -1 until
    // their sets arrive, at most kWaitSeconds. Logs "[GEARSETS] waiting for <name>'s gear sets".
    void OnLogin(APoplarPlayerController* pc);

    // Server: the client's gear-sets message on ServerProcessConvolve. Fills ReplicatedLoadouts
    // and NumLoadoutEntitlements. Logs "[GEARSETS] <name>: <n> gear sets in the hero screen row".
    void OnSetsReceived(APoplarPlayerController* pc, const nlohmann::json& message);

    // Server, every tick: a player whose sets did not come in time gets 0 sets (the screen opens
    // with the empty row, as before).
    void ServerTick(float dt);

    // Both ends: PoplarPlayerController.ServerSelectPlayerMetaLoadoutIndex. Client: remembers the
    // picked cell. Server: logs it.
    void OnSelectLoadout(APoplarPlayerController* pc, int index);

    // Client, four times a second: sends the save's sets once per connection (one message per
    // set; resent while the server shows no count), fills its own copy of the row once the
    // server's count arrives, and watches that the hero screen's loadout check can pass (if the
    // sets do not add up after a few seconds, it lowers the local count so the screen never stays
    // stuck).
    void ClientTick();

    // Client, when the hero screen closes and before SetGear: puts the picked set's three items
    // in Globals::GearSlotOne/Two/Three. Logs "[GEARSETS] using set ...".
    void ApplyPickedSet();

    // Client: the Gear Sets window (main menu, Command). Builds and saves the sets.
    void DrawEditor();
}

#include "GearSetsGame.hpp"

#include "Engine.hpp"
#include "Globals.hpp"
#include "Metagame.hpp"
#include "Networking.hpp"
#include "UiStyle.hpp"
#include "Utils.hpp"
#include "imgui/imgui.h"
#include "imgui/imgui_stdlib.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace GearSets {
    namespace {
        const uint64_t kPendingKill = 0x2000000000000000ull; // RF_PendingKill
        // Server: how long a player's hero screen may wait for their sets. A client without this
        // feature never sends any; after this the row opens empty.
        const float kWaitSeconds = 8.0f;
        // Client: how long the received sets may disagree with their count before the local
        // count is lowered (one replication update can bring the count before the sets).
        const float kMismatchSeconds = 4.0f;

        bool IsLive(UObject* o) { return o && !(o->ObjectFlags & kPendingKill); }

        std::string PlayerName(APoplarPlayerController* pc) {
            return pc && pc->MyPoplarPRI ? pc->MyPoplarPRI->PlayerName.ToString() : std::string("?");
        }

        APoplarMetaPlayerReplicationInfo* MetaOf(APoplarPlayerController* pc) {
            if (!IsLive(pc) || !IsLive(pc->MyPoplarPRI)) return nullptr;
            APoplarMetaPlayerReplicationInfo* meta = pc->MyPoplarPRI->GetMetaPRI();
            return IsLive(meta) ? meta : nullptr;
        }

        // Server: players whose sets have not come in yet, with the parts received so far.
        struct Waiting {
            APoplarPlayerController* pc;
            float seconds;
            std::vector<int> parts;     // part numbers received (a resend repeats them)
            std::vector<Set> sets;
        };
        std::vector<Waiting> g_waiting;

        Waiting* WaitingFor(APoplarPlayerController* pc) {
            for (Waiting& w : g_waiting) if (w.pc == pc) return &w;
            return nullptr;
        }

        void StopWaiting(APoplarPlayerController* pc) {
            std::erase_if(g_waiting, [pc](const Waiting& w) { return w.pc == pc; });
        }

        // An FString the engine can own (it frees and reallocates these with its own allocator).
        void SetEngineString(FString& s, const std::string& text) {
            std::wstring w(text.begin(), text.end());
            int count = (int)w.size() + 1;
            if (!s.ArrayData || s.ArrayMax < count) {
                // The old buffer (if any) is left alone: an engine buffer we did not allocate.
                s.ArrayData = (wchar_t*)Engine::EngineMalloc(sizeof(wchar_t) * count);
                s.ArrayMax = count;
            }
            memcpy((void*)s.ArrayData, w.c_str(), sizeof(wchar_t) * count);
            s.ArrayCount = count;
        }

        void ClearPerk(FReplicatedPerkItem& p) {
            p.PerkFunction = nullptr;
            p.Rarity = 0;
            p.ItemLevel = 0;
            p.bCanUse = 0;
            p.bActive = 0;
        }

        // Writes the sets into ReplicatedLoadouts[0..n) in order, leaving out gear that is unknown
        // or that usable() refuses, and sets with no gear left; returns n. Only entries that pass
        // the hero screen's check are written, so n is exactly what that check counts.
        template <typename Usable>
        int FillLoadouts(APoplarMetaPlayerReplicationInfo* meta, const std::vector<Set>& sets, const char* who, Usable usable) {
            int n = 0;
            for (const Set& s : sets) {
                if (n >= 0x4B) break;
                FMetaLoadoutInstance& L = meta->ReplicatedLoadouts[n];
                PerkFields f[3];
                std::string perksLog;
                for (int k = 0; k < 3; k++) {
                    FReplicatedPerkItem& p = L.Perks[k];
                    ClearPerk(p);
                    if (s.perks[k].empty()) { perksLog += " -"; continue; }
                    UPoplarPerkFunction* perk = UObject::FindObject<UPoplarPerkFunction>(s.perks[k]);
                    if (!IsLive(perk) || !usable(perk)) {
                        perksLog += perk ? " (not usable here)" : " (unknown)";
                        continue;
                    }
                    p.PerkFunction = perk;
                    p.Rarity = GameUtils::RarityStringToRarity(s.perks[k]);
                    p.ItemLevel = 0;
                    p.bCanUse = 1;
                    f[k] = { p.Rarity, true, p.ItemLevel };
                    perksLog += " " + perk->GetName();
                }
                if (!LoadoutCounts(f)) {
                    for (int k = 0; k < 3; k++) ClearPerk(L.Perks[k]);
                    printf("[GEARSETS] %s: set %d \"%s\" left out: no usable gear (%s )\n", who, s.index, s.name.c_str(), perksLog.c_str());
                    continue;
                }
                L.Index = s.index;
                L.Version = 1;
                SetEngineString(L.LoadoutName, s.name);
                printf("[GEARSETS] %s: row cell %d = set %d \"%s\":%s\n", who, n, s.index, s.name.c_str(), perksLog.c_str());
                n++;
            }
            return n;
        }

        // Client state, per connection (the local controller).
        bool g_filledLocally = false;
        APoplarPlayerController* g_sentFor = nullptr;
        int g_sends = 0;
        float g_sinceSend = 0.0f;
        const float kResendSeconds = 3.0f;
        int g_pickedCell = -1;
        float g_mismatchSeconds = 0.0f;
        bool g_countLowered = false;

        int ValidLoadouts(APoplarMetaPlayerReplicationInfo* meta) {
            int n = 0;
            for (int i = 0; i < 0x4B; i++) {
                PerkFields f[3];
                for (int k = 0; k < 3; k++) {
                    const FReplicatedPerkItem& p = meta->ReplicatedLoadouts[i].Perks[k];
                    f[k] = { p.Rarity, p.PerkFunction != nullptr, p.ItemLevel };
                }
                if (LoadoutCounts(f)) n++;
            }
            return n;
        }

        Metagame::SaveFile* CurrentSave() {
            if (Globals::CurrentSaveFile >= Globals::saveFiles.size()) return nullptr;
            return &Globals::saveFiles[Globals::CurrentSaveFile];
        }

        std::vector<Set> SetsFromSave(const Metagame::SaveFile& save) {
            std::vector<Set> sets;
            for (int i = 0; i < (int)save.loadouts.size() && i < kMaxSets; i++) {
                Set s;
                s.index = i;
                s.name = save.loadouts[i].name;
                for (int k = 0; k < 3; k++) s.perks[k] = save.loadouts[i].items[k].itemObjectName;
                sets.push_back(s);
            }
            return sets;
        }
    }

    void OnLogin(APoplarPlayerController* pc) {
        APoplarMetaPlayerReplicationInfo* meta = MetaOf(pc);
        if (!meta) {
            printf("[GEARSETS] %s has no meta PRI at login; no gear sets\n", PlayerName(pc).c_str());
            return;
        }
        int before = meta->NumLoadoutEntitlements;
        meta->NumLoadoutEntitlements = -1;
        StopWaiting(pc);
        g_waiting.push_back({ pc, 0.0f, {}, {} });
        printf("[GEARSETS] waiting for %s's gear sets (loadout count was %d, now -1; at most %.0f s)\n",
            PlayerName(pc).c_str(), before, kWaitSeconds);
    }

    void OnSetsReceived(APoplarPlayerController* pc, const nlohmann::json& message) {
        std::string name = PlayerName(pc);
        APoplarMetaPlayerReplicationInfo* meta = MetaOf(pc);
        if (!meta) {
            printf("[GEARSETS] %s sent gear sets but has no meta PRI; ignored\n", name.c_str());
            return;
        }
        Waiting* w = WaitingFor(pc);
        if (!w) {
            // The hero screen copies the row once; changing it now would only confuse the count.
            printf("[GEARSETS] %s sent gear sets again (row already has %d); ignored\n", name.c_str(), meta->NumLoadoutEntitlements);
            return;
        }
        int expected = PartsExpected(message);
        int part = message.contains("part") && message["part"].is_number_integer() ? message["part"].get<int>() : -1;
        if (expected < 0 || part < 0 || part >= (std::max)(expected, 1)) {
            printf("[GEARSETS] %s sent a malformed gear-sets message; ignored\n", name.c_str());
            return;
        }
        if (std::find(w->parts.begin(), w->parts.end(), part) == w->parts.end()) {
            w->parts.push_back(part);
            for (const Set& s : FromMessage(message))
                if (std::none_of(w->sets.begin(), w->sets.end(), [&](const Set& o) { return o.index == s.index; }))
                    w->sets.push_back(s);
        }
        if ((int)w->parts.size() < (std::max)(expected, 1)) return;   // more parts to come
        std::vector<Set> sets = w->sets;
        std::sort(sets.begin(), sets.end(), [](const Set& a, const Set& b) { return a.index < b.index; });
        StopWaiting(pc);

        UNetConnection* conn = reinterpret_cast<UNetConnection*>(pc->Player);
        // Only objects the client can resolve. (The perks do not reach the client anyway, see
        // ClientTick; the server's copy is what its own CurrentLoadout is taken from.)
        int n = FillLoadouts(meta, sets, name.c_str(), [conn](UPoplarPerkFunction* perk) {
            return conn && ServerNetworking::PackageMapSupportsObject(conn, perk);
        });
        meta->NumLoadoutEntitlements = n;
        printf("[GEARSETS] %s: %d gear sets in the hero screen row (%d sent)\n", name.c_str(), n, (int)sets.size());
    }

    void ServerTick(float dt) {
        for (Waiting& w : g_waiting) w.seconds += dt;
        std::erase_if(g_waiting, [](const Waiting& w) {
            if (!IsLive(w.pc)) return true;
            if (w.seconds < kWaitSeconds) return false;
            if (APoplarMetaPlayerReplicationInfo* meta = MetaOf(w.pc)) {
                if (meta->NumLoadoutEntitlements < 0) meta->NumLoadoutEntitlements = 0;
                printf("[GEARSETS] %s sent no gear sets in %.0f s; the row stays empty\n", PlayerName(w.pc).c_str(), kWaitSeconds);
            }
            return true;
        });
    }

    void OnSelectLoadout(APoplarPlayerController* pc, int index) {
        if (Globals::amServer) {
            APoplarMetaPlayerReplicationInfo* meta = MetaOf(pc);
            std::string setName = meta && index >= 0 && index < meta->NumLoadoutEntitlements
                ? meta->ReplicatedLoadouts[index].LoadoutName.ToString() : std::string("(none)");
            printf("[GEARSETS] %s picked row cell %d (%s)\n", PlayerName(pc).c_str(), index, setName.c_str());
            return;
        }
        if (index != g_pickedCell) printf("[GEARSETS] client: picked row cell %d\n", index);
        g_pickedCell = index;
    }

    void ClientTick() {
        if (Globals::amServer || Globals::amStandalone) return;
        APoplarPlayerController* pc = SDKUtils::GetLocalPlayerController();
        if (!IsLive(pc) || pc->Role != ENetRole::ROLE_AutonomousProxy || !IsLive(pc->MyPoplarPRI)) return;

        APoplarMetaPlayerReplicationInfo* meta = MetaOf(pc);
        // Sent once per connection, then again every few seconds (at most twice more) while the
        // server still shows no count; the server ignores parts it already has.
        if (pc != g_sentFor) {
            g_sentFor = pc;
            g_sends = 0;
            g_sinceSend = 0.0f;
            g_pickedCell = -1;
            g_mismatchSeconds = 0.0f;
            g_countLowered = false;
            g_filledLocally = false;
        }
        else {
            g_sinceSend += 0.25f;
        }
        bool due = g_sends == 0 || (g_sends < 3 && g_sinceSend >= kResendSeconds && (!meta || meta->NumLoadoutEntitlements < 0));
        if (due) {
            Metagame::SaveFile* save = CurrentSave();
            std::vector<std::string> msgs = ToMessages(save ? SetsFromSave(*save) : std::vector<Set>());
            size_t longest = 0;
            for (const std::string& msg : msgs) {
                std::wstring wmsg(msg.begin(), msg.end());
                pc->eventServerProcessConvolve(wmsg.c_str(), 0);
                longest = (std::max)(longest, msg.size());
            }
            g_sends++;
            g_sinceSend = 0.0f;
            printf("[GEARSETS] client: sent %d gear sets to the server (%zu messages, longest %zu bytes, try %d)\n",
                PartsExpected(nlohmann::json::parse(msgs[0])), msgs.size(), longest, g_sends);
            return;
        }

        // The server's count arrives, its perks do not (in run 20261101-014136 all three sets came
        // with no usable perk: the loadout's perk fields are not sent to clients). So once the
        // count is known (the server has the sets; the screen is still waiting, since 0 perks are
        // valid), this client writes the same sets from its own save into its own copy, and sets
        // the count to what it wrote. Same order and same rule as the server, so the row cell
        // the player picks is the server's cell too.
        if (meta && !g_filledLocally && meta->NumLoadoutEntitlements > 0) {
            g_filledLocally = true;
            const FMetaLoadoutInstance& first = meta->ReplicatedLoadouts[0];
            printf("[GEARSETS] client: server lists %d sets; arrived for cell 0: index %d, version %d, name \"%s\", perk 0 %s rarity %d level %d\n",
                meta->NumLoadoutEntitlements, first.Index, first.Version, first.LoadoutName.ToString().c_str(),
                first.Perks[0].PerkFunction ? "set" : "none", first.Perks[0].Rarity, first.Perks[0].ItemLevel);
            Metagame::SaveFile* save = CurrentSave();
            std::vector<Set> sets;
            if (save) for (const Set& s : SetsFromSave(*save)) if (HasGear(s)) sets.push_back(s);
            int n = FillLoadouts(meta, sets, "client", [](UPoplarPerkFunction*) { return true; });
            for (int i = n; i < meta->NumLoadoutEntitlements && i < 0x4B; i++)
                for (int k = 0; k < 3; k++) ClearPerk(meta->ReplicatedLoadouts[i].Perks[k]);
            if (n != meta->NumLoadoutEntitlements)
                printf("[GEARSETS] client: wrote %d sets, the server listed %d; showing %d\n", n, meta->NumLoadoutEntitlements, n);
            meta->NumLoadoutEntitlements = n;
            return;
        }

        // Safety net for the hero screen's check: the count must equal the valid sets received.
        if (!meta || g_countLowered || meta->NumLoadoutEntitlements < 0) { g_mismatchSeconds = 0.0f; return; }
        int valid = ValidLoadouts(meta);
        if (valid == meta->NumLoadoutEntitlements) { g_mismatchSeconds = 0.0f; return; }
        g_mismatchSeconds += 0.25f;
        if (g_mismatchSeconds < kMismatchSeconds) return;
        printf("[GEARSETS] client: the server listed %d gear sets but %d arrived usable; showing %d so the hero screen can open\n",
            meta->NumLoadoutEntitlements, valid, valid);
        meta->NumLoadoutEntitlements = valid;
        g_countLowered = true;
    }

    void ApplyPickedSet() {
        APoplarPlayerController* pc = SDKUtils::GetLocalPlayerController();
        APoplarMetaPlayerReplicationInfo* meta = MetaOf(pc);
        int cell = g_pickedCell;
        // No pick seen on the wire: the movie's own record of the selected cell.
        if (cell < 0) {
            if (UPoplarCharacterSelectGFxMovie* movie = SDKUtils::GetLastOfClass<UPoplarCharacterSelectGFxMovie>())
                if (IsLive(movie)) cell = movie->PlayerUISelections[0].SelectedLoadoutIndex;
        }
        Metagame::SaveFile* save = CurrentSave();
        if (!meta || !save || cell < 0 || cell >= meta->NumLoadoutEntitlements) {
            printf("[GEARSETS] client: no gear set picked (cell %d, %d in the row); gear left as it was\n", cell, meta ? meta->NumLoadoutEntitlements : -1);
            return;
        }
        // The cell names its save set (Index); checked against the gear the cell shows, and if
        // they disagree, the first save set with that gear is used.
        const FMetaLoadoutInstance& L = meta->ReplicatedLoadouts[cell];
        auto shows = [&](const Metagame::Loadout& set) {
            for (int k = 0; k < 3; k++) {
                UPoplarPerkFunction* perk = L.Perks[k].PerkFunction;
                if (perk && (!IsLive(perk) || perk->GetFullName() != set.items[k].itemObjectName)) return false;
            }
            return true;
        };
        int index = L.Index;
        if (index < 0 || index >= (int)save->loadouts.size() || !shows(save->loadouts[index])) {
            int found = -1;
            for (int i = 0; i < (int)save->loadouts.size() && found < 0; i++) if (shows(save->loadouts[i])) found = i;
            printf("[GEARSETS] client: row cell %d names set %d, which does not hold its gear; %s\n", cell, index,
                found >= 0 ? ("using set " + std::to_string(found) + " instead").c_str() : "gear left as it was");
            if (found < 0) return;
            index = found;
        }
        Metagame::Loadout& set = save->loadouts[index];
        Metagame::Item** slots[3] = { &Globals::GearSlotOne, &Globals::GearSlotTwo, &Globals::GearSlotThree };
        for (int k = 0; k < 3; k++) *slots[k] = set.items[k].itemObjectName.empty() ? nullptr : &set.items[k];
        printf("[GEARSETS] client: using set %d \"%s\" (row cell %d): %s | %s | %s\n", index, CleanName(set.name, index).c_str(), cell,
            set.items[0].itemDisplayName.c_str(), set.items[1].itemDisplayName.c_str(), set.items[2].itemDisplayName.c_str());
    }

    namespace {
        std::string Lower(std::string s) {
            for (char& c : s) c = (char)std::tolower((unsigned char)c);
            return s;
        }

        // One gear slot in the editor: a drop-down of the save's items with a filter box (not
        // case-sensitive, focused when the list opens). Items already in the set's other slots
        // are greyed out: a set holds three different items.
        void SlotCombo(const char* id, Metagame::Item& slot, const Metagame::Item& otherA, const Metagame::Item& otherB,
                       const std::vector<Metagame::Item>& items) {
            ImGui::PushID(id);
            ImGui::SetNextItemWidth(-1.0f);
            const char* shown = slot.itemObjectName.empty() ? "No Item" : slot.itemDisplayName.c_str();
            if (ImGui::BeginCombo("##slot", shown, ImGuiComboFlags_HeightLarge)) {
                UiStyle::WindowFont();   // the list is its own window, at the default size otherwise
                static std::string filter;
                if (ImGui::IsWindowAppearing()) { filter.clear(); ImGui::SetKeyboardFocusHere(); }
                ImGui::InputTextWithHint("##filter", "Type to filter", &filter);
                if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();   // the filter box eats Escape otherwise
                const std::string wanted = Lower(filter);
                if (ImGui::Selectable("No Item", slot.itemObjectName.empty())) slot = Metagame::Item();
                for (size_t i = 0; i < items.size(); i++) {
                    const Metagame::Item& item = items[i];
                    if (!wanted.empty() && Lower(item.itemDisplayName).find(wanted) == std::string::npos) continue;
                    // Several items share a display name; the index keeps each row's ID unique.
                    ImGui::PushID((int)i);
                    bool used = item.itemObjectName == otherA.itemObjectName || item.itemObjectName == otherB.itemObjectName;
                    if (used) ImGui::BeginDisabled();
                    bool picked = ImGui::Selectable(item.itemDisplayName.c_str(), slot.itemObjectName == item.itemObjectName);
                    if (used) ImGui::EndDisabled();
                    ImGui::PopID();
                    if (picked) slot = item;
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted(item.itemDisplayName.c_str());
                        ImGui::Separator();
                        ImGui::TextUnformatted(item.itemFlavor.c_str());
                        ImGui::EndTooltip();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }
    }

    void DrawEditor() {
        // Edits are made on a copy and written on Save, so Close throws them away.
        static std::vector<Metagame::Loadout> draft;
        static bool loaded = false;
        static int selected = 0;
        Metagame::SaveFile* save = CurrentSave();
        if (!Globals::GearViewerOpen || !save) { loaded = false; return; }
        if (!loaded) { draft = save->loadouts; loaded = true; selected = 0; }

        UiStyle::NextWindow(0.6f);
        ImGui::Begin("Gear Sets", &Globals::GearViewerOpen, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
        UiStyle::WindowFont();
        ImGui::TextUnformatted("Gear Sets");
        ImGui::TextWrapped("Each set is up to three different gear items. Pick a set in the game's loadout list when you choose your hero.");
        ImGui::Separator();

        for (int i = 0; i < (int)draft.size(); i++) {
            std::string label = CleanName(draft[i].name, i) + "##set" + std::to_string(i);
            if (ImGui::RadioButton(label.c_str(), selected == i)) selected = i;
            if (i % 4 != 3 && i + 1 < (int)draft.size()) ImGui::SameLine();
        }
        if ((int)draft.size() < kMaxSets && UiStyle::Button("+ New Set")) {
            Metagame::Loadout l;
            l.name = DefaultName((int)draft.size());
            draft.push_back(l);
            selected = (int)draft.size() - 1;
        }

        if (selected >= 0 && selected < (int)draft.size()) {
            Metagame::Loadout& l = draft[selected];
            ImGui::Separator();
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##name", &l.name);
            if (l.name.size() > kMaxNameLength) l.name.resize(kMaxNameLength);
            SlotCombo("one", l.items[0], l.items[1], l.items[2], save->items);
            SlotCombo("two", l.items[1], l.items[0], l.items[2], save->items);
            SlotCombo("three", l.items[2], l.items[0], l.items[1], save->items);
            if (UiStyle::Button("Delete This Set")) {
                draft.erase(draft.begin() + selected);
                if (selected >= (int)draft.size()) selected = (int)draft.size() - 1;
            }
        }

        ImGui::Separator();
        float w = UiStyle::ButtonWidth(2);
        if (UiStyle::Button("Save", w)) {
            save->loadouts = draft;
            for (int i = 0; i < (int)save->loadouts.size(); i++) save->loadouts[i].name = CleanName(save->loadouts[i].name, i);
            // Gear slots point into the save; they are picked again at the next hero screen.
            Globals::GearSlotOne = Globals::GearSlotTwo = Globals::GearSlotThree = nullptr;
            Metagame::WriteSave((int)Globals::CurrentSaveFile, *save);
            printf("[GEARSETS] saved %d gear sets\n", (int)save->loadouts.size());
            Globals::GearViewerOpen = false;
        }
        ImGui::SameLine();
        if (UiStyle::Button("Close", w)) Globals::GearViewerOpen = false;
        ImGui::End();
    }
}

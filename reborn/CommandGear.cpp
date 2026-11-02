#include "CommandGear.hpp"

#include "CommandGearRules.hpp"
#include "Engine.hpp"
#include "GearSets.hpp"
#include "Globals.hpp"
#include "Metagame.hpp"
#include "Utils.hpp"
#include "safetyhook.hpp"

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace CommandGear {
    namespace {
        using namespace CommandGearRules;

        // The client's request sender (UPoplarOnlineClientService send, battleborn+0x15beaa0):
        // (service, user, bIsGet, serviceId, path, body, completion function name, out request id).
        // Returns 0 when sent, else the ServiceResult (6 = "invalid service": no Hydra any more).
        SafetyHookInline g_send;

        // Engine helpers. The game's arrays are {int Num; int Max; T* Data} on its own heap.
        struct GameArray { int32_t num; int32_t max; uint8_t* data; };
        // New Max for a grown array (battleborn+0xa2df0), and resize of its data (battleborn+0x3c110).
        int Slack(int num, int max, int elemSize) {
            return reinterpret_cast<int (*)(int, int, int)>(Globals::baseAddress + 0xa2df0)(num, max, elemSize);
        }
        void Resize(uint8_t** data, int newMax, int elemSize) {
            reinterpret_cast<void (*)(uint8_t**, uint64_t, int, int)>(Globals::baseAddress + 0x3c110)(data, 0, newMax, elemSize);
        }
        uint8_t* AddSlot(GameArray* a, int elemSize) {
            int i = a->num++;
            if (a->num > a->max) {
                a->max = Slack(a->num, a->max, elemSize);
                Resize(&a->data, a->max, elemSize);
            }
            return a->data + (size_t)i * elemSize;
        }
        // UObject::FindFunction (battleborn+0x785d0).
        UFunction* FindFunction(UObject* o, FName name) {
            uint64_t raw;
            memcpy(&raw, &name, sizeof(raw));
            return reinterpret_cast<UFunction* (*)(UObject*, uint64_t, int)>(Globals::baseAddress + 0x785d0)(o, raw, 0);
        }
        // Calls each delegate of a copied list with the params, then frees the copy (battleborn+0x155510).
        void BroadcastCopy(UObject* service, GameArray* copy, void* params) {
            reinterpret_cast<void (*)(UObject*, GameArray*, void*)>(Globals::baseAddress + 0x155510)(service, copy, params);
        }

        // What a service's "done" delegates get: OnReceive...(PlayerID, ServiceResult).
        struct DoneParams { FPlatformUserId user; int32_t result; };

        void Broadcast(UObject* service, size_t delegatesOffset, FPlatformUserId user, int result) {
            GameArray* src = reinterpret_cast<GameArray*>(reinterpret_cast<uint8_t*>(service) + delegatesOffset);
            GameArray copy{ 0, 0, nullptr };
            if (src->num > 0) {
                copy.max = src->num;
                Resize(&copy.data, copy.max, 0x10);
                memcpy(copy.data, src->data, (size_t)src->num * 0x10);
                copy.num = src->num;
            }
            DoneParams p{ user, result };
            BroadcastCopy(service, &copy, &p);
        }

        // What the Gear page holds now (log only).
        void DumpPage() {
            UPoplarCommandArtifactsGFxMovie* page = SDKUtils::GetLastOfClass<UPoplarCommandArtifactsGFxMovie>();
            if (!page) { printf("[GEARTAB] gear page: not open\n"); return; }
            printf("[GEARTAB] gear page: bank %d, sideboard %d, loadouts %d, page state %d, request state %d\n",
                page->PerkBank.ArrayCount, page->Sideboard.ArrayCount, page->Loadouts.ArrayCount,
                (int)page->ArtifactPageState, (int)page->ArtifactRequestState);
            if (page->PerkBank.ArrayCount > 0) {
                const FReplicatedPerkItem& p = page->PerkBank.ArrayData[0];
                printf("[GEARTAB]   bank[0]: id %s key %s rarity %d level %d perk %s data %p json %s\n", p.MetaID.ToString().c_str(),
                    p.AssetKey.ToString().c_str(), p.Rarity, p.ItemLevel, p.PerkFunction ? p.PerkFunction->GetName().c_str() : "(none)",
                    (void*)p.ItemData.Dummy, p.ApolloJSON.ToString().c_str());
            }
            for (int i = 0; i < page->Loadouts.ArrayCount && i < 2; i++) {
                const FMetaLoadoutInstance& l = page->Loadouts.ArrayData[i];
                const FReplicatedPerkItem& p = l.Perks[0];
                printf("[GEARTAB]   loadout[%d]: index %d \"%s\" perk0 id %s perk %s rarity %d\n", i, l.Index, l.LoadoutName.ToString().c_str(),
                    p.MetaID.ToString().c_str(), p.PerkFunction ? p.PerkFunction->GetName().c_str() : "(none)", p.Rarity);
            }
        }

        // Answers a request as if the server had: the completion function the request named, with a
        // SparkResult carrying the reply (HTTP 200). It finds the user by the request id in the
        // service's ClientRequests, which Send already added.
        void Respond(UObject* service, FName callback, int requestId, const std::string& body, const char* what, int httpStatus = 200) {
            UFunction* fn = FindFunction(service, callback);
            if (!fn) {
                printf("[GEARTAB] %s: completion %s not found; not answered\n", what, callback.ToString().c_str());
                return;
            }
            FSparkResult r;
            memset(&r, 0, sizeof(r));
            r.RequestId = requestId;
            r.bRequestSucceeded = 1;
            r.HttpStatusCode = httpStatus;
            std::vector<uint8_t> bytes(body.begin(), body.end());
            r.ResponseBody.ArrayData = bytes.data();
            r.ResponseBody.ArrayCount = (int32_t)bytes.size();
            r.ResponseBody.ArrayMax = (int32_t)bytes.size();
            service->ProcessEvent(fn, &r, nullptr);
            // The body stays ours (the game copies it); never let the game free it.
            r.ResponseBody.ArrayData = nullptr;
            printf("[GEARTAB] %s: answered locally (%zu bytes, %s)\n", what, body.size(), callback.ToString().c_str());
            for (float t : { 2.0f, 15.0f }) Engine::RunOnGameThreadAfter(t, DumpPage);
        }

        Metagame::SaveFile* CurrentSave() {
            if (Globals::CurrentSaveFile >= Globals::saveFiles.size()) return nullptr;
            return &Globals::saveFiles[Globals::CurrentSaveFile];
        }

        // guid -> object name, for every item last sent to the Gear tab (saving maps back by it).
        std::map<std::string, std::string> g_guidToObject;
        // asset key -> object name. Gear bank entries get their own short ids from the game
        // (e.g. "3ACD9B46", not our guid), so an item moved from the bank is found by its key.
        std::map<std::string, std::string> g_keyToObject;

        // The object name behind an id the Gear tab sent back: one of our guids, or a bank entry's id.
        const std::string* ObjectForId(const std::string& id) {
            auto it = g_guidToObject.find(id);
            if (it != g_guidToObject.end()) return &it->second;
            UPoplarCommandArtifactsGFxMovie* page = SDKUtils::GetLastOfClass<UPoplarCommandArtifactsGFxMovie>();
            if (!page) return nullptr;
            for (int i = 0; i < page->PerkBank.ArrayCount; i++) {
                const FReplicatedPerkItem& p = page->PerkBank.ArrayData[i];
                if (p.MetaID.ToString() != id) continue;
                auto k = g_keyToObject.find(p.AssetKey.ToString());
                return k != g_keyToObject.end() ? &k->second : nullptr;
            }
            return nullptr;
        }

        std::string WideToUtf8(const wchar_t* w) {
            std::string out;
            if (!w) return out;
            for (; *w; w++) out += (*w < 0x80) ? (char)*w : '?';
            return out;
        }

        // Every piece of gear in the game, so any save can build sets from all of it: the save's own
        // items and set items first (they keep the save's names), then the rest of the game's gear.
        std::vector<GearItem> GearFromSave(const Metagame::SaveFile& save, int* unknownCount) {
            std::vector<const Metagame::Item*> source;
            std::set<std::string> seen;
            for (const Metagame::Item& i : save.items)
                if (!i.itemObjectName.empty() && seen.insert(i.itemObjectName).second) source.push_back(&i);
            for (const Metagame::Loadout& l : save.loadouts)
                for (const Metagame::Item& i : l.items)
                    if (!i.itemObjectName.empty() && seen.insert(i.itemObjectName).second) source.push_back(&i);
            static std::vector<Metagame::Item> gameGear;
            if (gameGear.size() < 700) {   // the full set is ~800; fewer means not all loaded yet
                gameGear.clear();
                for (UPoplarPerkFunction* perk : SDKUtils::GetAllOfClass<UPoplarPerkFunction>())
                    if (!perk->GetFullName().contains("Default")) gameGear.push_back(Metagame::Item(perk));
            }
            for (const Metagame::Item& i : gameGear)
                if (!i.itemObjectName.empty() && seen.insert(i.itemObjectName).second) source.push_back(&i);

            std::vector<GearItem> out;
            *unknownCount = 0;
            for (const Metagame::Item* i : source) {
                UPoplarPerkFunction* perk = UObject::FindObject<UPoplarPerkFunction>(i->itemObjectName);
                if (!perk) { (*unknownCount)++; continue; }
                GearItem g;
                g.guid = GuidFor(i->itemObjectName);
                g.name = i->itemDisplayName;
                g.description = i->itemDescription.empty() ? i->itemFlavor : i->itemDescription;
                g.assetKey = perk->AssetTrackerKey.ToString();
                g.rarity = RarityFromObjectName(i->itemObjectName);
                g.level = (int)i->level;
                g_guidToObject[g.guid] = i->itemObjectName;
                g_keyToObject[g.assetKey] = i->itemObjectName;
                out.push_back(g);
            }
            return out;
        }

        std::vector<GearLoadout> LoadoutsFromSave(const Metagame::SaveFile& save) {
            std::vector<GearLoadout> out;
            for (int i = 0; i < (int)save.loadouts.size() && i < GearSets::kMaxSets; i++) {
                GearLoadout l;
                l.id = LoadoutIdFor(i);
                l.name = GearSets::CleanName(save.loadouts[i].name, i);
                for (int k = 0; k < 3; k++) {
                    const std::string& obj = save.loadouts[i].items[k].itemObjectName;
                    l.perks[k] = obj.empty() ? "" : GuidFor(obj);
                }
                out.push_back(l);
            }
            return out;
        }

        std::string SideboardsBody(int* count) {
            Metagame::SaveFile* save = CurrentSave();
            int unknown = 0;
            std::vector<GearItem> items = save ? GearFromSave(*save, &unknown) : std::vector<GearItem>();
            *count = (int)items.size();
            if (!items.empty())
                printf("[GEARTAB] gear list: %d items (first: \"%s\", asset key \"%s\", %s)%s\n", (int)items.size(),
                    items[0].name.c_str(), items[0].assetKey.c_str(), RarityWord(items[0].rarity),
                    unknown ? (" " + std::to_string(unknown) + " unknown left out").c_str() : "");
            else
                printf("[GEARTAB] gear list: empty (%s)\n", save ? "the save has no gear" : "no save selected");
            if (!items.empty()) {
                // The game finds each item's perk by its asset key (battleborn+0x125b530).
                std::wstring key(items[0].assetKey.begin(), items[0].assetKey.end());
                UObject* found = reinterpret_cast<UObject* (*)(const wchar_t*)>(Globals::baseAddress + 0x125b530)(key.c_str());
                printf("[GEARTAB] asset key \"%s\" -> %s\n", items[0].assetKey.c_str(), found ? found->GetFullName().c_str() : "(not found)");
            }
            return SideboardsReply(items);
        }

        // saved: the set the game just saved, echoed back unchanged (EchoSaved).
        std::string MetaProfileBody(const SavedLoadout* saved = nullptr) {
            Metagame::SaveFile* save = CurrentSave();
            std::vector<GearLoadout> loadouts = save ? LoadoutsFromSave(*save) : std::vector<GearLoadout>();
            if (saved && saved->ok) EchoSaved(loadouts, *saved);
            int bank = 0;
            if (save) { int unknown = 0; bank = (int)GearFromSave(*save, &unknown).size(); }
            printf("[GEARTAB] loadouts: %d saved sets in save %u \"%s\", %d slots\n", (int)loadouts.size(), Globals::CurrentSaveFile,
                save ? save->name.c_str() : "(none)", GearSets::kMaxSets);
            return MetaProfileReply(loadouts, GearSets::kMaxSets, bank);
        }

        // The Gear tab saved a loadout: write it into the save's gear sets. Returns the request as
        // read (ok false when nothing was saved).
        SavedLoadout SaveLoadout(const std::string& body) {
            SavedLoadout l = ParseSaveRequest(body);
            Metagame::SaveFile* save = CurrentSave();
            int index = SaveIndexFor(l.id);
            if (!l.ok || !save || index < 0 || index >= GearSets::kMaxSets) {
                printf("[GEARTAB] save loadout: not saved (%s; %zu bytes)\n",
                    !l.ok ? "could not read the request" : !save ? "no save selected" : "loadout id out of range", body.size());
                l.ok = false;
                return l;   // still answered: the screen would only show an error
            }
            while ((int)save->loadouts.size() <= index) {
                Metagame::Loadout empty;
                empty.name = GearSets::DefaultName((int)save->loadouts.size());
                save->loadouts.push_back(empty);
            }
            Metagame::Loadout& set = save->loadouts[index];
            if (!l.name.empty()) set.name = GearSets::CleanName(l.name, index);
            std::string log;
            for (int k = 0; k < 3; k++) {
                set.items[k] = Metagame::Item();
                if (l.perks[k].empty()) { log += " -"; continue; }
                const std::string* object = ObjectForId(l.perks[k]);
                if (!object) { log += " (unknown " + l.perks[k] + ")"; continue; }
                for (const Metagame::Item& owned : save->items)
                    if (owned.itemObjectName == *object) { set.items[k] = owned; break; }
                if (set.items[k].itemObjectName.empty()) {
                    // In a set but not in the items list: keep it by name.
                    for (const Metagame::Loadout& other : save->loadouts)
                        for (const Metagame::Item& i : other.items)
                            if (i.itemObjectName == *object && set.items[k].itemObjectName.empty()) set.items[k] = i;
                }
                log += " " + (set.items[k].itemDisplayName.empty() ? *object : set.items[k].itemDisplayName);
            }
            // Gear slots point into the save; they are picked again at the next hero screen.
            Globals::GearSlotOne = Globals::GearSlotTwo = Globals::GearSlotThree = nullptr;
            Metagame::WriteSave((int)Globals::CurrentSaveFile, *save);
            printf("[GEARTAB] save loadout %d (set %d) \"%s\":%s\n", l.id, index, set.name.c_str(), log.c_str());
            return l;
        }

        // Safety stop for the page saving the same set over and over (EchoSaved is the fix): on the
        // third identical save in a row the page is told it is no longer leaving, so it stops.
        std::string g_lastSaveBody;
        int g_sameSaveCount = 0;
        void StopSaveLoop(const std::string& body) {
            g_sameSaveCount = body == g_lastSaveBody ? g_sameSaveCount + 1 : 1;
            g_lastSaveBody = body;
            if (g_sameSaveCount < 3) return;
            UPoplarCommandArtifactsGFxMovie* page = SDKUtils::GetLastOfClass<UPoplarCommandArtifactsGFxMovie>();
            if (!page) return;
            page->SaveLoadoutHandlerType = ECommandArtifactSaveLoadoutHandlerType::ARTIFACTSAVELOADOUTSTATE_None;
            printf("[GEARTAB] same loadout saved %d times in a row; stopped the page's save loop\n", g_sameSaveCount);
        }

        // The Command screen's requests, by path.
        enum class Kind { PassThrough, Sideboards, MetaProfile, SaveLoadout, Entitlements, UnlockSchedule, Marketplace, Stats };

        Kind Classify(const std::wstring& path) {
            if (path == L"/v1/poplar/client/sideboards") return Kind::Sideboards;
            if (path == L"/v1/poplar/client/meta_profiles") return Kind::MetaProfile;
            if (path == L"/v2/poplar/client/meta_profiles/loadouts") return Kind::SaveLoadout;
            if (path.starts_with(L"/v1/poplar/client/entitlements/first_party?")) return Kind::Entitlements;
            if (path == L"/v2/poplar/client/item_unlock_schedules") return Kind::UnlockSchedule;
            if (path == L"/v1/poplar/client/marketplace") return Kind::Marketplace;
            if (path == L"/v2/poplar/players/current/stats") return Kind::Stats;
            return Kind::PassThrough;
        }

        // Done-delegate lists (offsets into the service object, from the request functions).
        const size_t kEntitlementsDone = 0xd0;    // ReadPlayerEntitlementDataCompleteDelegates
        const size_t kUnlockScheduleDone = 0x140; // ReadItemUnlockScheduleDataCompleteDelegates
        const size_t kMarketplaceDone = 0x2b0;    // ItemService ReadPlayerMarketplaceDataCompleteDelegates
        const size_t kLoadoutsDone = 0x160;       // ReadPlayerLoadoutsCompleteDelegates
        const size_t kStatsDone = 0x2d8;          // ReadPlayerStatsDataCompleteDelegates
        const size_t kStatReadsInProgress = 0x2c8;

        int g_nextRequestId = 0x7f000000;
        std::set<std::wstring> g_passedPaths;

        void AddClientRequest(UObject* service, FPlatformUserId user, int requestId) {
            GameArray* requests = reinterpret_cast<GameArray*>(reinterpret_cast<uint8_t*>(service) + 0x58);
            uint8_t* slot = AddSlot(requests, sizeof(FClientRequest));
            FClientRequest r;
            r.ClientId = user;
            r.RequestId = requestId;
            memcpy(slot, &r, sizeof(r));
        }

        // The stats request marks the user as "reading" until its reply; no reply comes, so unmark.
        void ForgetStatRead(UObject* service, FPlatformUserId user) {
            GameArray* reads = reinterpret_cast<GameArray*>(reinterpret_cast<uint8_t*>(service) + kStatReadsInProgress);
            for (int i = 0; i < reads->num; i++) {
                if (memcmp(reads->data + (size_t)i * 8, &user, 4) != 0) continue;
                memmove(reads->data + (size_t)i * 8, reads->data + (size_t)(i + 1) * 8, (size_t)(reads->num - i - 1) * 8);
                reads->num--;
                return;
            }
        }

        int64_t SendHook(UObject* service, FPlatformUserId* user, bool isGet, int64_t serviceId, FString* path, FString* body,
                         uint64_t callbackRaw, int* outRequestId) {
            // The completion function's FName, passed by value (8 bytes). Declared as a number: the
            // SDK's FName is passed by pointer by our compiler, which read it as an address.
            FName callback;
            memcpy(&callback, &callbackRaw, sizeof(callback));
            static bool keysLogged = false;
            if (!keysLogged) {
                // The Command screen's previous/next tab keys (battleborn+0x13af110).
                keysLogged = true;
                const size_t keys[4] = { 0x3523188, 0x3522f10, 0x3523190, 0x3522f18 };
                std::string names;
                for (size_t k : keys) names += " " + reinterpret_cast<FName*>(Globals::baseAddress + k)->ToString();
                printf("[GEARTAB] Command tab keys (prev prev next next):%s\n", names.c_str());
            }
            std::wstring p = path && path->ArrayData && path->ArrayCount > 0 ? std::wstring(path->ArrayData) : std::wstring();
            Kind kind = Classify(p);
            if (kind == Kind::PassThrough || !service || !user || !(user->bHasValue) || !outRequestId) {
                if (g_passedPaths.insert(p).second)
                    printf("[GEARTAB] request %s left to the game (no local answer)\n", WideToUtf8(p.c_str()).c_str());
                return g_send.call<int64_t>(service, user, isGet, serviceId, path, body, callbackRaw, outRequestId);
            }

            int id = g_nextRequestId++;
            *outRequestId = id;
            FPlatformUserId u = *user;
            std::string what = WideToUtf8(p.c_str());
            switch (kind) {
            case Kind::Sideboards: {
                AddClientRequest(service, u, id);
                int count = 0;
                std::string reply = SideboardsBody(&count);
                Engine::RunOnGameThreadAfter(0.0f, [service, callback, id, reply, what]() { Respond(service, callback, id, reply, what.c_str()); });
                break;
            }
            case Kind::MetaProfile: {
                AddClientRequest(service, u, id);
                std::string reply = MetaProfileBody();
                Engine::RunOnGameThreadAfter(0.0f, [service, callback, id, reply, what]() { Respond(service, callback, id, reply, what.c_str()); });
                break;
            }
            case Kind::SaveLoadout: {
                // Its reply goes to ReadPlayerCharacterLoadoutsComplete (battleborn+0x15a4a90), which
                // reads meta_profile.loadouts like the profile reply and stores them before telling the
                // page. Answered with the sets as saved, or the page puts the old gear back. Its result
                // code is the reply's HTTP status (battleborn+0x15c0ab0) and the page shows any code
                // but 0 as an error box, so this reply says status 0; the handler does not check it.
                std::string text = body && body->ArrayData ? WideToUtf8(body->ArrayData) : std::string();
                SavedLoadout saved = SaveLoadout(text);
                StopSaveLoop(text);
                AddClientRequest(service, u, id);
                std::string reply = MetaProfileBody(&saved);
                Engine::RunOnGameThreadAfter(0.0f, [service, callback, id, reply, what]() { Respond(service, callback, id, reply, what.c_str(), 0); });
                break;
            }
            default: {
                size_t done = kind == Kind::Entitlements ? kEntitlementsDone
                    : kind == Kind::UnlockSchedule ? kUnlockScheduleDone
                    : kind == Kind::Marketplace ? kMarketplaceDone : kStatsDone;
                bool stats = kind == Kind::Stats;
                Engine::RunOnGameThreadAfter(0.0f, [service, u, done, stats, what]() {
                    if (stats) ForgetStatRead(service, u);
                    Broadcast(service, done, u, 0);
                    printf("[GEARTAB] %s: reported done\n", what.c_str());
                });
                break;
            }
            }
            return 0;
        }
    }

    void InstallHooks() {
        g_send = safetyhook::create_inline((void*)(Globals::baseAddress + 0x15beaa0), &SendHook);
        if (!g_send) printf("[GEARTAB] hook FAILED: request sender\n");
    }
}

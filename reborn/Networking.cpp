#include "Networking.hpp"

#include <algorithm>
#include <map>
#include <psapi.h>

namespace ServerNetworking {
    std::string GetDateTimeISO() {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        std::stringstream ss;
        ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%S");
        ss << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';

        return ss.str();
    }

    void GameControllerPoll() {
        nlohmann::json jsonBody = nlohmann::json();

        int numConnectedPlayers = 0;
        for (Globals::ServerPlayer& serverPlayer: Globals::ServerPlayers) {
            if (serverPlayer.Connection)
                numConnectedPlayers++;
        }

        jsonBody["ConnectedPlayers"] = numConnectedPlayers;
        jsonBody["HumansHaveStarted"] = Globals::haveHumansStarted;

        Globals::GameCoordinatorHttpClient.get()->Post("/api/games/server-poll", jsonBody.dump(), "application/json");

        nlohmann::json telemetrySnapshotPoll = nlohmann::json();

        telemetrySnapshotPoll["Timestamp"] = GetDateTimeISO();

        telemetrySnapshotPoll["MatchGUID"] = ServerSettings::GameCoordinatorKey;

        telemetrySnapshotPoll["TargetTickrate"] = ServerSettings::tickrate;

        float averageTickrate = 0.0f;

        {
            std::scoped_lock t(Globals::Telemetry::TickrateMutex);

            float sum = std::accumulate(Globals::Telemetry::Tickrates.begin(), Globals::Telemetry::Tickrates.end(), 0.0f);

            // No samples since the last poll (no net driver yet): 0, not 0/0 = NaN.
            averageTickrate = Globals::Telemetry::Tickrates.empty() ? 0.0f : sum / Globals::Telemetry::Tickrates.size();

            Globals::Telemetry::Tickrates.clear();
        }

        telemetrySnapshotPoll["EffectiveTickrate"] = averageTickrate;

        {
            std::scoped_lock t(Globals::Telemetry::ReplicationFNameMutex);
            telemetrySnapshotPoll["ReplicationFNames"] = Globals::Telemetry::ReplicationFNames;
        }

        telemetrySnapshotPoll["PlayersConnected"] = numConnectedPlayers;

        telemetrySnapshotPoll["MaxPlayers"] = ServerSettings::NumPlayersToStart;

        PROCESS_MEMORY_COUNTERS_EX pmc;

        GetProcessMemoryInfo(GetCurrentProcess(),
            (PROCESS_MEMORY_COUNTERS*)&pmc,
            sizeof(pmc));

        telemetrySnapshotPoll["MemoryUsageMB"] = (int)(pmc.WorkingSetSize / (1024.0 * 1024.0));

        Globals::GameCoordinatorHttpClient.get()->Post("/api/telemetry/snapshot", telemetrySnapshotPoll.dump(), "application/json");

        {
            std::scoped_lock t(Globals::Telemetry::ReplicationFNameMutex);

            Globals::Telemetry::ReplicationFNames.clear();
        }
    }

    // The retail client build answers every incoming connection with ACCEPTC_Reject:
    // UWorld::NotifyAcceptingConnection (battleborn+0x45adb0) is just "xor eax,eax; ret".
    // UTcpNetDriver::TickDispatch (battleborn+0xc21f80) only creates a connection when that
    // call returns 1, so the client's hello is read and dropped before any control message
    // exists. The server swaps that vtable slot for one that accepts.
    int NotifyAcceptingConnectionAccept(void* notify) {
        static int logged = 0;
        if (logged < 5) { logged++; printf("[NETWORKING] accepting an incoming connection\n"); }
        return 1; // ACCEPTC_Accept
    }

    void ForceAcceptConnections() {
        static bool done = false;
        if (done) return;
        done = true;

        void** slot = reinterpret_cast<void**>(Globals::baseAddress + 0x27c2330); // UWorld's FNetworkNotify vtable, slot 0
        void* expected = reinterpret_cast<void*>(Globals::baseAddress + 0x45adb0);
        if (*slot != expected) {
            printf("[NETWORKING] NotifyAcceptingConnection slot holds %p, expected %p; not patching\n", *slot, expected);
            return;
        }

        DWORD old = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
            printf("[NETWORKING] could not unprotect NotifyAcceptingConnection slot (error %lu)\n", GetLastError());
            return;
        }
        *slot = reinterpret_cast<void*>(&NotifyAcceptingConnectionAccept);
        VirtualProtect(slot, sizeof(void*), old, &old);
        printf("[NETWORKING] NotifyAcceptingConnection patched to accept\n");
    }

    // UWorld::Listen normally follows the driver's InitListen with BuildServerMasterMap(), which is
    // NetDriver->MasterMap->AddNetPackages(): one FPackageInfo per loaded package that has net
    // objects, then Compute() to fill the name->index map. The mod calls the driver's InitListen
    // directly and never did that, so the master map stayed empty. UWorld::WelcomePlayer
    // (battleborn+0x45b060) copies the master map into each connection's PackageMap; with an
    // empty map UPackageMap::ObjectToIndex (battleborn+0x1f0f0) returns INDEX_NONE for every
    // class, GetClassNetCache (battleborn+0x1e310) returns NULL, and UActorChannel::SetChannelActor
    // (battleborn+0x611970) reads [NULL+0x20] at +0x1b8 on the very first replicated actor.
    void BuildServerMasterMap(UTcpNetDriver* NetDriver) {
        uintptr_t driver = reinterpret_cast<uintptr_t>(NetDriver);
        uintptr_t masterMap = *reinterpret_cast<uintptr_t*>(driver + 0x90); // UNetDriver::MasterMap, read at driver+0x90 by WelcomePlayer
        if (!masterMap) {
            printf("[NETWORKING] net driver has no master package map; replication will crash\n");
            return;
        }

        void** vtable = *reinterpret_cast<void***>(masterMap);
        void* addNetPackages = vtable[0x238 / 8]; // UPackageMap::AddNetPackages (battleborn+0x1eb40); it calls Compute() (slot 0x230) itself
        printf("[NETWORKING] building master package map, AddNetPackages at battleborn+0x%llx\n",
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(addNetPackages) - Globals::baseAddress));

        reinterpret_cast<void (*)(void*)>(addNetPackages)(reinterpret_cast<void*>(masterMap));

        int numPackages = *reinterpret_cast<int*>(masterMap + 0x58); // UPackageMap::List.Num (TArray here is {Num, Max, Data})
        printf("[NETWORKING] master package map has %i packages\n", numPackages);

        // UPackageMap::Compute (battleborn+0x1e8d0, slot 0x230) only puts a package into the
        // name->index map that ObjectToIndex (battleborn+0x1f0f0) searches when its FPackageInfo
        // has RemoteGeneration (+0x28) > 0. The FPackageInfo ctor (battleborn+0x1dda0) leaves it 0;
        // the engine would normally set it from the client's NMT_Have replies, but this client
        // build's UWorld::NotifyControlMessage (battleborn+0x45c540) only handles the client-side
        // messages (Failure, Uses, Unload, DebugText), so NMT_Have is ignored and the map that
        // UPackageMap::Copy (slot 0x250) hands to every connection stays empty: SupportsObject
        // fails for every class and nothing can be replicated. Client and server are the same
        // build, so mark every package as present on the remote side at its local generation.
        uintptr_t list = *reinterpret_cast<uintptr_t*>(masterMap + 0x60); // UPackageMap::List.Data, 0x50 bytes per FPackageInfo
        int marked = 0;
        for (int i = 0; i < numPackages && list; i++) {
            uintptr_t info = list + static_cast<uintptr_t>(i) * 0x50;
            int localGeneration = *reinterpret_cast<int*>(info + 0x24);
            if (localGeneration > 0) {
                *reinterpret_cast<int*>(info + 0x28) = localGeneration; // RemoteGeneration
                marked++;
            }
        }

        reinterpret_cast<void (*)(void*)>(vtable[0x230 / 8])(reinterpret_cast<void*>(masterMap)); // UPackageMap::Compute

        int numMapped = *reinterpret_cast<int*>(masterMap + 0x68); // UPackageMap::PackageListMap pairs Num
        bool supportsWorldInfo = reinterpret_cast<bool (*)(void*, UObject*)>(vtable[0x210 / 8])(reinterpret_cast<void*>(masterMap), AWorldInfo::StaticClass());
        printf("[NETWORKING] master package map: %i packages marked present remotely, %i in the name map, supports Engine.WorldInfo: %s\n",
            marked, numMapped, supportsWorldInfo ? "yes" : "no");
    }

    bool PackageMapSupportsObject(UNetConnection* connection, UObject* object) {
        void* packageMap = connection->PackageMap;
        if (!packageMap || !object)
            return false;

        void** vtable = *reinterpret_cast<void***>(packageMap);
        return reinterpret_cast<bool (*)(void*, UObject*)>(vtable[0x210 / 8])(packageMap, object); // UPackageMap::SupportsObject (battleborn+0x1f1e0)
    }

    void LogConnectionPackageMap(UNetConnection* connection) {
        uintptr_t map = reinterpret_cast<uintptr_t>(connection ? connection->PackageMap : nullptr);
        if (!map) {
            printf("[NETWORKING] connection has no package map\n");
            return;
        }

        printf("[NETWORKING] connection package map: %i packages, %i in the name map, supports Engine.WorldInfo: %s\n",
            *reinterpret_cast<int*>(map + 0x58), *reinterpret_cast<int*>(map + 0x68),
            PackageMapSupportsObject(connection, AWorldInfo::StaticClass()) ? "yes" : "no");
    }

    // UPackageMap::SupportsPackage (slot 0x218): the per-package half of the check
    // UNetConnection makes for on-demand packages (battleborn+0x61b5f0, slot 0x2a8).
    bool PackageMapSupportsPackage(UNetConnection* connection, UObject* package) {
        void* packageMap = connection ? connection->PackageMap : nullptr;
        if (!packageMap || !package)
            return false;

        void** vtable = *reinterpret_cast<void***>(packageMap);
        return reinterpret_cast<bool (*)(void*, UObject*)>(vtable[0x218 / 8])(packageMap, package);
    }

    // The same three steps BuildServerMasterMap does, for any package map: pick up packages that
    // were loaded after the map was built (a character's GD_*_Streaming package only loads on the
    // server when that character is selected, long after listen), mark them present on the
    // remote side (this server never processes the client's NMT_Have, see BuildServerMasterMap),
    // and recompute the name->index map that SupportsObject/SupportsPackage search.
    int RefreshPackageMap(void* mapObject, const char* label) {
        uintptr_t map = reinterpret_cast<uintptr_t>(mapObject);
        if (!map)
            return 0;

        void** vtable = *reinterpret_cast<void***>(map);
        int before = *reinterpret_cast<int*>(map + 0x58);

        reinterpret_cast<void (*)(void*)>(vtable[0x238 / 8])(mapObject); // UPackageMap::AddNetPackages; packages already listed are skipped

        int numPackages = *reinterpret_cast<int*>(map + 0x58);
        uintptr_t list = *reinterpret_cast<uintptr_t*>(map + 0x60);
        int marked = 0;
        for (int i = 0; i < numPackages && list; i++) {
            uintptr_t info = list + static_cast<uintptr_t>(i) * 0x50;
            int localGeneration = *reinterpret_cast<int*>(info + 0x24);
            if (localGeneration > 0) {
                *reinterpret_cast<int*>(info + 0x28) = localGeneration; // RemoteGeneration
                marked++;
            }
        }

        reinterpret_cast<void (*)(void*)>(vtable[0x230 / 8])(mapObject); // UPackageMap::Compute

        printf("[NETWORKING] %s package map refreshed: %i -> %i packages, %i marked present remotely, %i in the name map\n",
            label, before, numPackages, marked, *reinterpret_cast<int*>(map + 0x68));
        return numPackages - before;
    }

    // Object references go over the wire as (NetIndex << 14) | position of the package in the
    // package map's list (UPackageMap::ObjectToIndex battleborn+0x1f0f0, IndexToObject +0x1f2f0),
    // so the client's list must hold the same package at the same position. WelcomePlayer sends
    // the whole list once (UNetConnection::SendPackageMap battleborn+0x61afa0, one NMT_Uses per
    // entry); packages AddNetPackages appends later are only known to the server unless they are
    // sent too. Without this the pawn's archetype (GD_<Hero>_Streaming...) resolves to None on the
    // client, the pawn's actor channel never spawns an actor, and ClientRestart(pawn) arrives as
    // ClientRestart(None). The client's world handles NMT_Uses mid-game (battleborn+0x45c540):
    // it appends the entry (AddPackageInfo, slot 0x248) and links it to its loaded package.
    static void SendNewPackageInfos(UNetConnection* connection, int from) {
        uintptr_t map = reinterpret_cast<uintptr_t>(connection ? connection->PackageMap : nullptr);
        if (!map)
            return;

        int num = *reinterpret_cast<int*>(map + 0x58);
        uintptr_t list = *reinterpret_cast<uintptr_t*>(map + 0x60);
        if (!list || from >= num)
            return;

        auto sendPackageInfo = reinterpret_cast<void (*)(UNetConnection*, void*)>(Globals::baseAddress + 0x61b370); // UNetConnection::SendPackageInfo
        std::string names;
        for (int i = from; i < num; i++) {
            uintptr_t info = list + static_cast<uintptr_t>(i) * 0x50;
            sendPackageInfo(connection, reinterpret_cast<void*>(info));
            if (i - from < 60)
                names += (i > from ? ", " : "") + std::to_string(i) + "=" + reinterpret_cast<FName*>(info)->ToString();
        }
        printf("[NETWORKING] sent %i new package infos (NMT_Uses) to connection %p, list positions %i-%i: %s\n", num - from, (void*)connection, from, num - 1, names.c_str());
    }

    static int PackageMapCount(void* mapObject) {
        return mapObject ? *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(mapObject) + 0x58) : 0; // UPackageMap::List.Num
    }

    static void* MasterPackageMap() {
        return Globals::netDriver ? *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(Globals::netDriver) + 0x90) : nullptr; // UNetDriver::MasterMap
    }

    // A package map entry's Parent must be a live object before UPackageMap::Compute (it reads the
    // package's generation table and name) runs over the list.
    static bool IsLiveObject(void* p) {
        __try {
            int index = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(p) + 0x30); // UObject::ObjectInternalInteger
            TArray<UObject*>* objects = UObject::GObjObjects();
            return objects && objects->ArrayData && index >= 0 && index < objects->ArrayCount && objects->ArrayData[index] == p;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Same package in two FPackageInfo entries: name (+0x0) and GUID (+0x10), the test
    // UPackageMap::AddPackageInfo (battleborn+0x1ef60) uses.
    static bool SamePackageInfo(uintptr_t a, uintptr_t b) {
        return memcmp(reinterpret_cast<void*>(a), reinterpret_cast<void*>(b), 8) == 0
            && memcmp(reinterpret_cast<void*>(a + 0x10), reinterpret_cast<void*>(b + 0x10), 16) == 0;
    }

    // Object references go out as positions in the connection's package list, and the client's list
    // mirrors that list entry for entry (whole list at welcome, then each NMT_Uses appended). So a
    // connection's list may only GROW AT THE END. AddNetPackages (battleborn+0x1eb40) must never run
    // on a connection map: it empties the list and rebuilds it in the engine's current net-package
    // order, which shifts every entry after a package the server unloaded. Run 20261007-053702: the
    // master map went 1045 -> 1019 after on-demand unloads, both connection maps were then rebuilt to
    // 1046 and only position 1045 was sent; positions 1019-1044 meant different packages on the
    // server and on the clients, and both clients crashed together (0xC0000005 battleborn+0xe8ac23).
    //
    // This appends, through UPackageMap::AddPackageInfo (slot 0x248, battleborn+0x1ef60: an entry
    // with the same name+GUID is only updated in place, otherwise a copy is appended; then Compute),
    // every master-map entry the connection does not list yet, in the master's order, and sends
    // exactly the appended entries. The master's entries are already marked present remotely
    // (RefreshPackageMap), so the copies go into the name map. Returns how many were appended.
    static int AppendMissingPackages(UNetConnection* connection, const char* label) {
        void* master = MasterPackageMap();
        void* mapObject = connection ? connection->PackageMap : nullptr;
        if (!master || !mapObject || mapObject == master)
            return 0;
        uintptr_t map = reinterpret_cast<uintptr_t>(mapObject);
        uintptr_t masterList = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(master) + 0x60);
        int masterNum = PackageMapCount(master);
        int before = PackageMapCount(mapObject);
        if (!masterList || masterNum <= 0)
            return 0;

        // Entries whose package object is gone: clear the dangling Parent first (Compute skips an
        // entry with no Parent; the position stays, so nothing shifts).
        int cleared = 0;
        uintptr_t list = *reinterpret_cast<uintptr_t*>(map + 0x60);
        for (int i = 0; i < before && list; i++) {
            void** parent = reinterpret_cast<void**>(list + static_cast<uintptr_t>(i) * 0x50 + 0x8);
            if (*parent && !IsLiveObject(*parent)) {
                *parent = nullptr;
                cleared++;
            }
        }

        void** vtable = *reinterpret_cast<void***>(map);
        auto addPackageInfo = reinterpret_cast<void (*)(void*, void*)>(vtable[0x248 / 8]); // UPackageMap::AddPackageInfo
        int updated = 0;
        for (int m = 0; m < masterNum; m++) {
            uintptr_t info = masterList + static_cast<uintptr_t>(m) * 0x50;
            int num = PackageMapCount(mapObject);
            list = *reinterpret_cast<uintptr_t*>(map + 0x60); // reallocated by appends
            int found = -1;
            for (int i = 0; i < num && list; i++) {
                if (SamePackageInfo(list + static_cast<uintptr_t>(i) * 0x50, info)) {
                    found = i;
                    break;
                }
            }
            if (found >= 0) {
                // Listed already. A package that was unloaded and loaded again is a new object:
                // AddPackageInfo points the existing entry (same position) at it.
                if (*reinterpret_cast<void**>(list + static_cast<uintptr_t>(found) * 0x50 + 0x8) == *reinterpret_cast<void**>(info + 0x8))
                    continue;
                updated++;
            }
            addPackageInfo(mapObject, reinterpret_cast<void*>(info));
        }
        if (cleared && !updated && PackageMapCount(mapObject) == before)
            reinterpret_cast<void (*)(void*)>(vtable[0x230 / 8])(mapObject); // UPackageMap::Compute

        int after = PackageMapCount(mapObject);
        if (after != before || updated || cleared)
            printf("[NETWORKING] %s package map appended: %i -> %i packages (master %i), %i entries re-pointed at a reloaded package, %i dangling package pointers cleared, %i in the name map\n",
                label, before, after, masterNum, updated, cleared, *reinterpret_cast<int*>(map + 0x68));
        if (after < before) {
            printf("[NETWORKING] WARNING: %s package list shrank (%i -> %i); not sending anything\n", label, before, after);
            return 0;
        }
        SendNewPackageInfos(connection, before);
        return after - before;
    }

    // A player is "in the match" once it has spawned: its controller has a live pawn, or its hero has
    // been applied (PSI.PlayerClass, set by SwitchToPendingPlayerClass right before the pawn spawns;
    // it stays set while the player is dead between lives). A player in character select has neither.
    static bool IsInMatch(UNetConnection* c) {
        constexpr uint64_t kPendingKill = 0x2000000000000000ull; // same flag as PawnUtils::kPendingKill
        APlayerController* pc = c ? c->Actor : nullptr;
        if (!pc || (pc->ObjectFlags & kPendingKill))
            return false;
        APawn* p = pc->Pawn;
        if (p && !(p->ObjectFlags & kPendingKill) && !p->bDeleteMe)
            return true;
        if (pc->IsA(APoplarPlayerController::StaticClass())) {
            APoplarPlayerStateInfo* psi = reinterpret_cast<APoplarPlayerController*>(pc)->PoplarPSI;
            if (psi && psi->PlayerClass)
                return true;
        }
        return false;
    }

    // Every joined player's connection that is IN THE MATCH (live, has its controller, welcomed with a
    // package list, has spawned). A player still in character select is never touched: its map stays
    // at its welcome size until its own spawn, whose package gate (Hooks.cpp step 3) then fails and
    // catches the map up with everything loaded by then, its own skin and taunt included. Run
    // 20261007-055347: the first player's spawn appended 254 packages to the other player's map while
    // that player was in character select; its class gate then passed early, so its own spawn never
    // refreshed the map, and SwitchToPendingPlayerClass(Class_DeathBlade) applied nothing 40 times
    // ("client has package yes", pawn none) until the run timed out.
    static std::vector<UNetConnection*> JoinedConnections() {
        std::vector<UNetConnection*> out;
        for (Globals::ServerPlayer& sp : Globals::ServerPlayers) {
            UNetConnection* c = sp.Connection;
            if (!c || !IsLiveConnection(c) || !c->Actor || !c->PackageMap || PackageMapCount(c->PackageMap) <= 0)
                continue;
            if (!IsInMatch(c))
                continue;
            if (std::find(out.begin(), out.end(), c) == out.end())
                out.push_back(c);
        }
        return out;
    }

    static std::string PlayerNameForConnection(UNetConnection* c) {
        for (Globals::ServerPlayer& sp : Globals::ServerPlayers) {
            if (sp.Connection == c)
                return sp.Name;
        }
        return "unknown player";
    }

    // A spawn: the spawning player's map gets the packages loaded since its welcome (its hero), and
    // every other joined player's map gets them too. Run 20261007-042539: only the spawning player's
    // map was refreshed, so c2 (spawned first, 1041 entries) never got GD_DeathBlade_DefaultSkin that
    // c1's later spawn loaded; the server wrote c1's skin as None for c2 and c1's Rath had no body on
    // c2. Every map is append-only (AppendMissingPackages), so the clients' lists stay aligned.
    void RefreshServerPackageMaps(UNetConnection* connection) {
        void* master = MasterPackageMap();
        if (master)
            RefreshPackageMap(master, "master");
        if (connection && connection->PackageMap) {
            char label[160];
            snprintf(label, sizeof(label), "spawning player %s's connection %p", PlayerNameForConnection(connection).c_str(), (void*)connection);
            AppendMissingPackages(connection, label);
        }
        for (UNetConnection* c : JoinedConnections()) {
            if (c == connection)
                continue;
            char label[160];
            snprintf(label, sizeof(label), "other player %s's connection %p (in the match; a player is spawning)", PlayerNameForConnection(c).c_str(), (void*)c);
            AppendMissingPackages(c, label);
        }
        for (Globals::ServerPlayer& sp : Globals::ServerPlayers) {
            UNetConnection* c = sp.Connection;
            if (!c || c == connection || !IsLiveConnection(c) || !c->Actor || IsInMatch(c))
                continue;
            printf("[NETWORKING] other player %s's connection %p is not in the match yet (no pawn, no hero applied): package map left at %i; its own spawn catches it up\n",
                sp.Name.c_str(), (void*)c, PackageMapCount(c->PackageMap));
        }
    }

    // Server, once per new player pawn: its skin (SkinData.SkinDef, what gives the pawn its body on
    // the clients) must be in every joined player's package map, or that client receives None and
    // the pawn has no body there. The skin's package can load after the spawn's package refresh (a
    // substituted default skin loads at SwitchToPendingPlayerClass), so check, and catch every map
    // up (append-only) when one is missing it. The engine re-sends a reference it could not map.
    void SyncNewPawnSkin(APawn* pawn, const char* who) {
        if (!pawn || !pawn->IsA(APoplarPawn::StaticClass()) || !Globals::netDriver)
            return;
        UObject* skin = reinterpret_cast<APoplarPawn*>(pawn)->SkinData.SkinDef;
        if (!skin)
            return;
        std::vector<UNetConnection*> joined = JoinedConnections();
        std::string missing;
        for (UNetConnection* c : joined) {
            if (!PackageMapSupportsObject(c, skin))
                missing += (missing.empty() ? "" : ", ") + PlayerNameForConnection(c) + " (" + std::to_string(PackageMapCount(c->PackageMap)) + " packages)";
        }
        static int logged = 0;
        if (missing.empty()) {
            if (logged < 20) {
                logged++;
                printf("[NETWORKING] skin check: %s's new pawn skin %s is in all %zu in-match players' package maps\n",
                    who, skin->GetFullName().c_str(), joined.size());
            }
            return;
        }
        printf("[NETWORKING] skin check: %s's new pawn skin %s is NOT in the package map of: %s; catching every in-match player's map up\n",
            who, skin->GetFullName().c_str(), missing.c_str());
        void* master = MasterPackageMap();
        if (master)
            RefreshPackageMap(master, "master (a new pawn's skin)");
        for (UNetConnection* c : joined) {
            char label[160];
            snprintf(label, sizeof(label), "player %s's connection %p (a new pawn's skin)", PlayerNameForConnection(c).c_str(), (void*)c);
            AppendMissingPackages(c, label);
        }
        for (UNetConnection* c : joined) {
            if (!PackageMapSupportsObject(c, skin))
                printf("[NETWORKING] skin check: %s's skin %s still not in %s's package map (%i packages)\n",
                    who, skin->GetFullName().c_str(), PlayerNameForConnection(c).c_str(), PackageMapCount(c->PackageMap));
        }
    }

    // Client side: how many packages the connection's package map lists, how many are not linked
    // to a loaded package (objects in them deserialize as None), and how many NMT_Uses the client
    // queued as pending (connection +0x5fe0, see battleborn+0x45c540).
    std::string ClientPackageMapSummary() {
        std::string out;
        for (UNetConnection* conn : SDKUtils::GetAllOfClass<UNetConnection>()) {
            if (!conn || conn->GetName().rfind("Default__", 0) == 0 || !conn->PackageMap)
                continue;
            uintptr_t map = reinterpret_cast<uintptr_t>(conn->PackageMap);
            int num = *reinterpret_cast<int*>(map + 0x58);
            uintptr_t list = *reinterpret_cast<uintptr_t*>(map + 0x60);
            int unlinked = 0;
            std::string unlinkedNames;
            for (int i = 0; i < num && list; i++) {
                uintptr_t info = list + static_cast<uintptr_t>(i) * 0x50;
                if (*reinterpret_cast<void**>(info + 0x8) == nullptr) {
                    if (unlinked < 8)
                        unlinkedNames += (unlinked ? ", " : "") + std::to_string(i) + "=" + reinterpret_cast<FName*>(info)->ToString();
                    unlinked++;
                }
            }
            int pending = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(conn) + 0x5fe0);
            out += (out.empty() ? "" : "; ") + conn->GetName() + ": " + std::to_string(num) + " packages, last "
                + (num > 0 && list ? reinterpret_cast<FName*>(list + static_cast<uintptr_t>(num - 1) * 0x50)->ToString() : std::string("none"))
                + ", unlinked " + std::to_string(unlinked) + (unlinked ? " (" + unlinkedNames + ")" : "") + ", pending uses " + std::to_string(pending);
        }
        return out.empty() ? std::string("no connection") : out;
    }

    // UNetConnection keeps the on-demand packages a client has reported loaded as a TArray<FName>
    // at +0x6048 ({Num, Max, Data}); ServerUpdateOnDemandPackageStatus (battleborn+0xe65c30)
    // adds to it and the connection's on-demand check (battleborn+0x61b5f0) searches it.
    static TArray<FName>* OnDemandPackageList(UNetConnection* connection) {
        return connection ? reinterpret_cast<TArray<FName>*>(reinterpret_cast<char*>(connection) + 0x6048) : nullptr;
    }

    bool ConnectionHasOnDemandPackage(UNetConnection* connection, UObject* package) {
        TArray<FName>* list = OnDemandPackageList(connection);
        if (!list || !package || !list->ArrayData)
            return false;

        for (int i = 0; i < list->ArrayCount; i++) {
            if (list->ArrayData[i].FNameEntryId == package->Name.FNameEntryId && list->ArrayData[i].InstanceNumber == package->Name.InstanceNumber)
                return true;
        }
        return false;
    }

    void MarkOnDemandPackageLoaded(UNetConnection* connection, UObject* package) {
        TArray<FName>* list = OnDemandPackageList(connection);
        if (!list || !package)
            return;

        // TArray<FName>::AddUniqueItem (battleborn+0x949f0), the call UpdateOnDemandPackageStatus
        // itself makes, so the array grows through the engine's allocator.
        reinterpret_cast<int (*)(void*, const FName*)>(Globals::baseAddress + 0x0949f0)(list, &package->Name);
    }

    std::string OnDemandPackageListString(UNetConnection* connection) {
        TArray<FName>* list = OnDemandPackageList(connection);
        if (!list)
            return "(no connection)";

        std::string s;
        for (int i = 0; i < list->ArrayCount && list->ArrayData && i < 40; i++) {
            if (i) s += ", ";
            s += list->ArrayData[i].ToString();
        }
        if (list->ArrayCount > 40) s += ", ...";
        return s.empty() ? "(empty)" : s;
    }

    void InitListen() {
        ForceAcceptConnections();

        SDKUtils::GetLastOfClass<UGameEngine>()->CreateNamedNetDriver(FName(020724));

        UTcpNetDriver* NetDriver = SDKUtils::GetLastOfClass<UTcpNetDriver>();

        UWorld* theWorld = *reinterpret_cast<UWorld**>(Globals::baseAddress + 0x34DFCA0);

        theWorld->NetDriver = NetDriver;

        SDKUtils::GetLastOfClass<AWorldInfo>()->NetMode = ENetMode::NM_DedicatedServer;

        FURL furl = FURL();

        furl.Port = ServerSettings::gamePort;

        FString error = FString(L"");

        printf("[NETWORKING] Normal init status %i\n", reinterpret_cast<char (*)(UNetDriver * NetDriver, size_t world, FURL & url, FString & error)>(Globals::baseAddress + 0x0c21e30)(NetDriver, (__int64)theWorld + 0x58, furl, error));

        theWorld->NetDriver = NetDriver;

        printf("[NETWORKING] net driver %s, bound port %i\n", NetDriver->GetFullName().c_str(), furl.Port);

        BuildServerMasterMap(NetDriver);

        NetDriver->NetConnectionClass = UTcpipConnection::StaticClass();

        {
            std::scoped_lock l(Globals::NetworkObjectListMutex);

            Globals::NetworkObjectList = SDKUtils::GetAllOfClass<AActor>();
        }

        Globals::netDriver = NetDriver;

        printf("[NETWORKING] Game networking listening on port %i!\n", ServerSettings::gamePort);
    }

    UActorChannel* GetActorChannelForActor(AActor* actor, UNetConnection* connection) {
        if (connection) {
            for (UChannel* channel : connection->Channels) {
                if (channel) {
                    if (channel->Class == UActorChannel::StaticClass() && ((UActorChannel*)channel)->Actor == actor) {
                        return (UActorChannel*)channel;
                    }
                }
            }
        }

        return nullptr;
    }

    // UNetDriver::ClientConnections, TArray<UNetConnection*> at driver+0x70 (count +0x70, data +0x78):
    // UTcpNetDriver::TickDispatch appends every accepted connection there (battleborn+0xc22234) and
    // looks incoming packets up in it (battleborn+0xc224c0). UNetConnection::CleanUp takes a closed
    // connection out of it, and the object is freed soon after. A ServerPlayer keeps its connection
    // pointer after that: the NotifyDisconnect hook never fires on the server, and TickNetServer's
    // state check needs connection->Actor, which CleanUp has already cleared. At match end both
    // clients left, their connections were freed, and the next DestroyActor walked the freed
    // connection's channel table (crash:0xc0000005:reborn>battleborn+0x401e66, run 20261005-054440:
    // GetActorChannelForActor read channel 0x1). So only a connection still in this list is used.
    bool IsLiveConnection(UNetConnection* connection) {
        if (!connection || !Globals::netDriver)
            return false;
        auto* list = reinterpret_cast<TArray<UNetConnection*>*>(reinterpret_cast<uintptr_t>(Globals::netDriver) + 0x70);
        if (!list->ArrayData)
            return false;
        for (int i = 0; i < list->ArrayCount; i++) {
            if (list->ArrayData[i] == connection)
                return true;
        }
        return false;
    }

    void ForgetClosedConnections(const char* where) {
        if (!Globals::netDriver)
            return;
        for (Globals::ServerPlayer& serverPlayer : Globals::ServerPlayers) {
            if (serverPlayer.Connection && !IsLiveConnection(serverPlayer.Connection)) {
                printf("[NETWORKING] %s's connection %p was closed (no longer in the net driver's connection list, noticed in %s); forgetting it\n",
                    serverPlayer.Name.c_str(), (void*)serverPlayer.Connection, where);
                serverPlayer.Connection = nullptr;
            }
        }
    }

    // A channel queued for closing is only touched while a live player connection still holds it
    // (compared by pointer, so a channel freed with its connection is never read).
    bool IsChannelOnLiveConnection(UActorChannel* channel) {
        if (!channel)
            return false;
        for (Globals::ServerPlayer& serverPlayer : Globals::ServerPlayers) {
            UNetConnection* connection = serverPlayer.Connection;
            if (!connection || !IsLiveConnection(connection))
                continue;
            for (UChannel* ch : connection->Channels) {
                if (ch == channel)
                    return true;
            }
        }
        return false;
    }

    // Helpers for the [MINIONS] logging below (read-only).
    static double NowSeconds() { return GetTickCount64() / 1000.0; }

    static UObject* OutermostOf(UObject* o) {
        while (o && o->Outer) o = o->Outer;
        return o;
    }

    static std::string NameOrNoneOf(UObject* o) { return o ? o->GetName() : std::string("None"); }

    // [MINIONS] server, every few seconds: the non-player pawns in WorldInfo.PawnList by archetype,
    // and per player whether each has an actor channel and whether that player's package map knows
    // its archetype; plus the minion spawners' state. Compare with the client's [MINIONS] line:
    // none here = the server never spawned them; here with channels but none on the client = they
    // did not replicate.
    void LogMinionCensus() {
        AWorldInfo* wi = nullptr;
        std::vector<UNetConnection*> conns;
        for (Globals::ServerPlayer& sp : Globals::ServerPlayers) {
            UNetConnection* c = sp.Connection;
            if (!c || !IsLiveConnection(c))
                continue;
            conns.push_back(c);
            if (!wi && c->Actor && !(c->Actor->ObjectFlags & 0x2000000000000000ull))
                wi = c->Actor->WorldInfo;
        }
        if (!wi)
            return;

        struct Group { int count = 0; std::string cls, pkg; std::vector<int> channels; std::vector<bool> inMap; };
        std::map<std::string, Group> groups;
        int total = 0, players = 0, guard = 0;
        for (APawn* p = wi->PawnList; p && guard < 2000; p = p->NextPawn, guard++) {
            total++;
            if (p->ObjectFlags & 0x2000000000000000ull || p->bDeleteMe)
                continue;
            if (p->Controller && p->Controller->IsA(APlayerController::StaticClass())) {
                players++;
                continue;
            }
            std::string key = p->ObjectArchetype ? p->ObjectArchetype->GetName() : p->Class->GetName();
            Group& g = groups[key];
            if (!g.count) {
                g.cls = p->Class->GetName();
                g.pkg = NameOrNoneOf(OutermostOf(p->ObjectArchetype));
                g.channels.assign(conns.size(), 0);
                for (UNetConnection* c : conns)
                    g.inMap.push_back(p->ObjectArchetype ? PackageMapSupportsObject(c, p->ObjectArchetype) : true);
            }
            g.count++;
            for (size_t i = 0; i < conns.size(); i++)
                if (GetActorChannelForActor(p, conns[i]))
                    g.channels[i]++;
        }

        std::string line;
        for (auto& [name, g] : groups) {
            line += (line.empty() ? "" : "; ") + std::to_string(g.count) + " " + name + " (" + g.cls + ", package " + g.pkg;
            for (size_t i = 0; i < conns.size(); i++)
                line += ", " + PlayerNameForConnection(conns[i]) + ": channels " + std::to_string(g.channels[i]) + "/" + std::to_string(g.count)
                    + " archetype in map " + (g.inMap[i] ? "yes" : "NO");
            line += ")";
        }

        int spawners = 0, permitted = 0, active = 0, spawned = 0;
        std::string waves;
        for (APoplarMinionSpawner* s : SDKUtils::GetAllOfClass<APoplarMinionSpawner>()) {
            if (!s || (s->ObjectFlags & 0x2000000000000000ull) || !s->Outer || !s->Outer->IsA(ULevel::StaticClass()))
                continue;
            spawners++;
            permitted += s->HasPermissionToSpawn ? 1 : 0;
            active += s->bActiveSpawn ? 1 : 0;
            spawned += s->bHasSpawned ? 1 : 0;
            if (spawners <= 6)
                waves += (waves.empty() ? "" : ",") + std::to_string(s->CurrentMinionWaveIndex);
        }
        int matchState = (wi->GRI && wi->GRI->IsA(APoplarGameReplicationInfo::StaticClass()))
            ? (int)reinterpret_cast<APoplarGameReplicationInfo*>(wi->GRI)->CurrentMatchState.State : -1;

        char head[300];
        snprintf(head, sizeof head, "[MINIONS] server: match state %i, PawnList %i pawns (%i players'), %i others: ", matchState, total, players, total - players);
        char tail[300];
        snprintf(tail, sizeof tail, " | minion spawners %i (permission %i, active %i, has spawned %i, wave index %s)",
            spawners, permitted, active, spawned, waves.empty() ? "-" : waves.c_str());
        std::string out = std::string(head) + (line.empty() ? "none" : line) + tail;

        static std::string last;
        static double lastAt = -100.0;
        double now = NowSeconds();
        if (out != last || now - lastAt >= 30.0) {
            printf("%s\n", out.c_str());
            last = out;
            lastAt = now;
        }
    }

    // [MINIONS] server, every half second: each non-player pawn's life, to tell why the Dojo's waves
    // last under 10 s with nobody shooting (live run 20261007-071315: 1 -> 4 -> 0 between two 5 s
    // censuses). Logs when one appears (where, health, physics, nearest player, the level's KillZ),
    // when its health first reaches 0, when it is torn off, and when it leaves the PawnList (last
    // seen place, lowest Z, health, physics). Died = health 0; fell = Z near or under KillZ;
    // culled/despawned = left with health and no fall.
    void TrackMinionLives() {
        struct Life { std::string name; double born = 0.0; float x = 0, y = 0, z = 0, minZ = 1e9f, vz = 0, health = 0, maxHealth = 0, nearest = -1;
                      int physics = 0; bool tornOff = false, hidden = false, loggedDead = false, loggedTearOff = false, seen = false; };
        static std::map<APawn*, Life> lives;

        AWorldInfo* wi = nullptr;
        std::vector<APawn*> playerPawns;
        for (Globals::ServerPlayer& sp : Globals::ServerPlayers) {
            UNetConnection* c = sp.Connection;
            if (!c || !IsLiveConnection(c) || !c->Actor || (c->Actor->ObjectFlags & 0x2000000000000000ull))
                continue;
            if (!wi) wi = c->Actor->WorldInfo;
            APawn* pp = c->Actor->Pawn;
            if (pp && !(pp->ObjectFlags & 0x2000000000000000ull) && !pp->bDeleteMe)
                playerPawns.push_back(pp);
        }
        if (!wi) {
            lives.clear();
            return;
        }
        double now = NowSeconds();
        for (auto& [p, l] : lives) l.seen = false;

        int guard = 0;
        for (APawn* p = wi->PawnList; p && guard < 2000; p = p->NextPawn, guard++) {
            if ((p->ObjectFlags & 0x2000000000000000ull) || p->bDeleteMe || (p->Controller && p->Controller->IsA(APlayerController::StaticClass())))
                continue;
            if (std::find(playerPawns.begin(), playerPawns.end(), p) != playerPawns.end())
                continue;
            float nearest = -1.0f;
            for (APawn* pp : playerPawns) {
                float dx = pp->Location.X - p->Location.X, dy = pp->Location.Y - p->Location.Y, dz = pp->Location.Z - p->Location.Z;
                float d = sqrtf(dx * dx + dy * dy + dz * dz);
                if (nearest < 0.0f || d < nearest) nearest = d;
            }
            auto it = lives.find(p);
            bool isNew = it == lives.end();
            Life& l = lives[p];
            l.seen = true;
            if (isNew) {
                l.name = p->ObjectArchetype ? p->ObjectArchetype->GetName() : p->Class->GetName();
                l.born = now;
            }
            l.x = p->Location.X; l.y = p->Location.Y; l.z = p->Location.Z; l.vz = p->Velocity.Z;
            l.minZ = (std::min)(l.minZ, p->Location.Z);
            l.health = p->GetHealth(); l.maxHealth = p->GetMaxHealth();
            l.physics = (int)p->Physics; l.tornOff = p->bTearOff; l.hidden = p->bHidden; l.nearest = nearest;
            if (isNew)
                printf("[MINIONS] server: + %s %p at (%.0f, %.0f, %.0f) health %.0f/%.0f physics %i controller %s, nearest player %.0f units, KillZ %.0f\n",
                    l.name.c_str(), (void*)p, l.x, l.y, l.z, l.health, l.maxHealth, l.physics, NameOrNoneOf(p->Controller).c_str(), nearest, wi->KillZ);
            if (l.health <= 0.0f && !l.loggedDead) {
                l.loggedDead = true;
                printf("[MINIONS] server: %s %p health 0 after %.1f s at (%.0f, %.0f, %.0f) physics %i, nearest player %.0f units\n",
                    l.name.c_str(), (void*)p, now - l.born, l.x, l.y, l.z, l.physics, nearest);
            }
            if (l.tornOff && !l.loggedTearOff) {
                l.loggedTearOff = true;
                printf("[MINIONS] server: %s %p torn off after %.1f s (health %.0f)\n", l.name.c_str(), (void*)p, now - l.born, l.health);
            }
        }

        for (auto it = lives.begin(); it != lives.end();) {
            Life& l = it->second;
            if (l.seen) { ++it; continue; }
            printf("[MINIONS] server: - %s %p left the PawnList after %.1f s: last at (%.0f, %.0f, %.0f) vz %.0f, lowest Z %.0f (KillZ %.0f), health %.0f/%.0f, physics %i, torn off %u, hidden %u, nearest player %.0f units\n",
                l.name.c_str(), (void*)it->first, now - l.born, l.x, l.y, l.z, l.vz, l.minZ, wi->KillZ, l.health, l.maxHealth, l.physics,
                (unsigned)l.tornOff, (unsigned)l.hidden, l.nearest);
            it = lives.erase(it);
        }
    }

    std::vector<AActor*> BuildConsiderList(AWorldInfo* WorldInfo, UNetDriver* NetDriver) {
        std::vector<AActor*> copiedNetworkObjectList;
        std::vector<AActor*> ret = std::vector<AActor*>();

        {
            std::scoped_lock l(Globals::NetworkObjectListMutex);

            copiedNetworkObjectList = Globals::NetworkObjectList;
        }

        for (AActor* actor : copiedNetworkObjectList) {
            if (!actor || actor->RemoteRole == ENetRole::ROLE_None || !actor->WorldInfo || actor->bPendingDelete || actor->ObjectFlags & 0x2000000000000000 || actor->Class == AEmitter::StaticClass()) { //
                continue;
            }
            else {
                if (actor->bAlwaysRelevant || actor->bForceNetUpdate || actor->bPendingNetUpdate || Globals::time - actor->LastNetUpdateTime > 1.0f / actor->NetUpdateFrequency) {
                    actor->LastNetUpdateTime = Globals::time;
                    ret.push_back(actor);
                }
            }
        }

        return ret;

       
    }

    uint8_t GetConnectionState(UNetConnection* connection) {
        return *(uint8_t*)((__int64)connection + 0x98);
    }

    bool CompareActorPriority(AActor* a, AActor* b) {
        int aPrio = 0;
        int bPrio = 0;

        /*
        * Ugly as shit atm, don't feel like making it better
        */

        if (a->IsA<APlayerController>()) {
            aPrio = 10;
        }

        if (b->IsA<APlayerController>()) {
            bPrio = 10;
        }

        if (a->IsA<APawn>()) {
            aPrio = 9;
        }

        if (b->IsA<APawn>()) {
            bPrio = 9;
        }

        if (a->IsA<AReplicationInfo>()) {
            aPrio = 8;
        }

        if (b->IsA<AReplicationInfo>()) {
            bPrio = 8;
        }

        return aPrio > bPrio;
    }

    bool ConnectionFull(UNetConnection* connection) {
        for (UChannel* Channel : connection->Channels) {
            if (!Channel) {
                return false;
            }
        }

        return true;
    }

    void TickNetServer(UTcpNetDriver* NetDriver) {
        static AWorldInfo* worldInfo = nullptr;

        if (!worldInfo)
            worldInfo = SDKUtils::GetLastOfClass<AWorldInfo>();

        ForgetClosedConnections("TickNetServer");

        std::vector<AActor*> actors = BuildConsiderList(worldInfo, NetDriver);

        //std::sort(actors.begin(), actors.end(), CompareActorPriority);
        
        for (Globals::ServerPlayer& serverPlayer: Globals::ServerPlayers) {
            UNetConnection* connection = serverPlayer.Connection;

            if (!connection)
                continue;

            if (!serverPlayer.shouldReplicateTo)
                continue;

            if (GetConnectionState(connection) < 3) {
                if (connection->Actor) {
                    serverPlayer.Connection = nullptr;
                    std::cout << "[NETWORKING] Player disconnected!" << std::endl;
                }

                continue;
            }

            if (!(*reinterpret_cast<bool(**)(UNetConnection*, bool)>(*(__int64*)connection + 0x260))(connection, 1))
                continue;

            if (connection->Actor && connection->Actor->PendingAdjustment.TimeStamp > 0.0) {
                connection->Actor->eventSendClientAdjustment();
            }

            for (AActor* actor : actors) {
                if (!actor)
                    continue;

                if (actor->ObjectFlags & 0x2000000000000000)
                    continue;

                if (!(*reinterpret_cast<bool(**)(UNetConnection*, bool)>(*(__int64*)connection + 0x260))(connection, 1))
                    continue;

                if (actor->IsA<APlayerController>() && (connection->Actor != actor)) {
                    continue;
                }

                bool markedAsNoGC = false;

                if (!(actor->ObjectFlags & 0x0000008000000000)) {
                    actor->ObjectFlags |= 0x0000008000000000;
                    markedAsNoGC = true;
                }

                if (actor->bNetTemporary || actor->bTearOff) {
                    bool shouldContinue = false;

                    for (AActor* cmpActor : serverPlayer.SentTemporaries) {
                        if (actor == cmpActor) {
                            shouldContinue = true;
                            break;
                        }
                    }

                    if (!shouldContinue) {
                        serverPlayer.SentTemporaries.push_back(actor);
                    }

                    if (shouldContinue) {
                        continue;
                    }
                }

                //printf("[NETWORKING] Starting the replication run for %s\n", actor->GetFullName().c_str());

                (*(void(__fastcall**)(UNetConnection*, AActor*))(*(__int64*)connection + 624LL))(connection, actor);

                UActorChannel* channel = GetActorChannelForActor(actor, connection);

                if (!channel && actor && !(actor->ObjectFlags & 0x2000000000000000) && (*reinterpret_cast<bool(**)(UNetConnection*, bool)>(*(__int64*)connection + 0x260))(connection, 1)) {
                    //printf("[NETWORKING] No channel for %s, creating...\n", actor->GetFullName().c_str());

                    // SetChannelActor dereferences GetClassNetCache(actor->Class) without a NULL check,
                    // and that is NULL whenever the connection's package map does not know the class.
                    if (!PackageMapSupportsObject(connection, actor->Class)) {
                        static int unsupportedLogged = 0;
                        if (unsupportedLogged < 20) {
                            unsupportedLogged++;
                            printf("[NETWORKING] not replicating %s: its class is not in the connection's package map\n", actor->GetFullName().c_str());
                        }
                    }
                    else {
                        channel = reinterpret_cast<UActorChannel * (__thiscall*)(UNetConnection * connection, int channelType, uint32_t openedLocally, int chIndex)>(Globals::baseAddress + 0x061daa0)(connection, 2, 1, -1);

                        if (channel) { //&& (*reinterpret_cast<bool(**)(UNetConnection*, bool)>(*(__int64*)connection + 0x260))(connection, 1)
                            //printf("[NETWORKING] Setting channel actor...\n");
                            reinterpret_cast<void(__thiscall*)(UActorChannel*, AActor*)>(Globals::baseAddress + 0x0611970)(channel, actor);
                        }
                    }
                }


                if (channel && channel->Actor && !(actor->ObjectFlags & 0x2000000000000000) && (*reinterpret_cast<bool(**)(UNetConnection*, bool)>(*(__int64*)connection + 0x260))(connection, 1) && channel->NumOutRec < 0xFE) {
                    //printf("[NETWORKING] Replication time!\n");
                    {
                        std::scoped_lock t(Globals::Telemetry::ReplicationFNameMutex);
                    
                        Globals::Telemetry::ReplicationFNames.push_back(channel->Actor->Class->Name.FNameEntryId);
                    }

                    reinterpret_cast<void (*)(UActorChannel * channel)>(Globals::baseAddress + 0x0613050)(channel);
                    if (channel->Actor) {
                        channel->Actor->NetTag++;
                    }
                }

                if (markedAsNoGC && actor) {
                    actor->ObjectFlags &= ~(0x0000008000000000);
                }
            }
        }

        /*
        std::random_device rd;
        std::mt19937 gen(rd());

        std::shuffle(Globals::connections.begin(), Globals::connections.end(), gen);
        */

        {
            std::lock_guard<std::mutex> lock(Globals::mutex);

            //Globals::DisableGC = false;

            while (!Globals::channelsToClose.empty()) {
                UActorChannel* ch = Globals::channelsToClose.back();

                Globals::channelsToClose.pop_back();

                if (IsChannelOnLiveConnection(ch) && ch->Connection && ch->Actor) {
                    reinterpret_cast<void (*)(UActorChannel* channel)>(Globals::baseAddress + 0x0613050)(ch);
                    (*(reinterpret_cast<void(**)(UActorChannel*)>(*(__int64*)ch + 0x210)))(ch);
                }
            }

            // Globals::DisableGC = true;
        }
    }
}

namespace ClientNetworking {
    // Client side. The world's NMT_Uses handler (battleborn+0x45c540) appends the server's entry to
    // the package map (AddPackageInfo, slot 0x248) unlinked, and links it with
    // UWorld::VerifyPackageInfo (battleborn+0x45b260) only when the connection's queue of pending
    // package infos (TArray of 0x50-byte FPackageInfo at connection+0x5fe0/+0x5fe8) is empty;
    // otherwise the entry goes to the back of that queue. UWorld::Tick pops the queue only from the
    // front, while VerifyPackageInfo succeeds. In this seek-free build VerifyPackageInfo starts an
    // async load for a package the client has not loaded and returns false until that finishes, so
    // the first bot hero package the client has never loaded (C031_PlagueBringer_* with 8 bots)
    // blocks every later entry for minutes, including the player's own, already-loaded
    // GD_<Hero>_Streaming. Objects in an unlinked entry deserialize as None (IndexToObject
    // battleborn+0x1f2f0 needs the entry's Parent), so the pawn's actor channel opens with a None
    // archetype, the client never spawns the pawn, and every ClientRestart(pawn) arrives as None.
    // VerifyPackageInfo needs nothing from the queue order: for a loaded package with the same GUID
    // it sets the generation, sends NMT_Have, and updates the map entry by name+GUID. So link every
    // queued entry whose package is already loaded, out of order, and drop it from the queue
    // (TArray RemoveAt battleborn+0x1ff80, which the NMT_Unload handler uses on the same array).
    int LinkLoadedPendingPackages(UWorld* world, bool onlyNewest, const char* why) {
        if (!world || Globals::amServer)
            return 0;
        if (*reinterpret_cast<uint8_t*>(Globals::baseAddress + 0x34a0b7f) == 0) {
            // VerifyPackageInfo's non-seek-free branch can load packages synchronously; leave it alone.
            static bool logged = false;
            if (!logged) { logged = true; printf("[NET] not a seek-free build; leaving queued package infos to the engine\n"); }
            return 0;
        }

        uintptr_t netDriver = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(world) + 0x128); // UWorld::NetDriver
        if (!netDriver)
            return 0;
        uintptr_t conn = *reinterpret_cast<uintptr_t*>(netDriver + 0x80); // UNetDriver::ServerConnection
        if (!conn)
            return 0;
        int* pendingNum = reinterpret_cast<int*>(conn + 0x5fe0);
        if (*pendingNum <= 0)
            return 0;

        auto findPackage = reinterpret_cast<UObject* (*)(UObject*, const wchar_t*)>(Globals::baseAddress + 0x879f0); // UObject::FindPackage
        auto verifyPackageInfo = reinterpret_cast<uint8_t (*)(UWorld*, void*)>(Globals::baseAddress + 0x45b260);
        auto removeAt = reinterpret_cast<void (*)(void*, int)>(Globals::baseAddress + 0x1ff80);

        int linked = 0;
        std::string names;
        int last = *pendingNum - 1;
        int first = onlyNewest ? last : 0;
        for (int i = last; i >= first && i < *pendingNum; i--) {
            uintptr_t info = *reinterpret_cast<uintptr_t*>(conn + 0x5fe8) + static_cast<uintptr_t>(i) * 0x50;
            const FName* fname = reinterpret_cast<const FName*>(info);
            std::string name = fname->ToString();
            if (fname->InstanceNumber > 0)
                name += "_" + std::to_string(fname->InstanceNumber - 1);
            std::wstring wname(name.begin(), name.end());

            UObject* pkg = findPackage(nullptr, wname.c_str());
            if (!pkg)
                continue;

            // Only the path VerifyPackageInfo links without loading or failing: package loaded, GUID
            // set and equal to the server's (UPackage GUID at +0x68, FPackageInfo GUID at +0x10).
            const uint32_t* pkgGuid = reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(pkg) + 0x68);
            const uint32_t* infoGuid = reinterpret_cast<const uint32_t*>(info + 0x10);
            if (!(pkgGuid[0] | pkgGuid[1] | pkgGuid[2] | pkgGuid[3]) || memcmp(pkgGuid, infoGuid, 16) != 0)
                continue;

            if (!verifyPackageInfo(world, reinterpret_cast<void*>(info)))
                continue;

            removeAt(pendingNum, i);
            if (linked < 12)
                names += (linked ? ", " : "") + name;
            linked++;
        }

        if (linked) {
            static int logged = 0;
            if (logged < 60) {
                logged++;
                std::string head = "none";
                if (*pendingNum > 0)
                    head = reinterpret_cast<const FName*>(*reinterpret_cast<uintptr_t*>(conn + 0x5fe8))->ToString();
                printf("[NET] %s: linked %i queued package info(s) whose package is already loaded, out of order: %s%s; still queued %i (head %s)\n",
                    why, linked, names.c_str(), linked > 12 ? ", ..." : "", *pendingNum, head.c_str());
            }
        }
        return linked;
    }

    // [MINIONS] client (and solo), every few seconds: the non-player pawns in this game's
    // WorldInfo.PawnList by archetype, and the server connection's actor channels, counting those
    // with no actor (an opening bunch whose archetype did not resolve leaves the channel without an
    // actor for good). Compare with the server's [MINIONS] line.
    void LogMinionCensus() {
        APoplarPlayerController* pc = SDKUtils::GetLocalPlayerController();
        if (!pc || !pc->WorldInfo || (pc->WorldInfo->ObjectFlags & 0x2000000000000000ull))
            return;
        AWorldInfo* wi = pc->WorldInfo;

        std::map<std::string, int> groups;
        int total = 0, players = 0, guard = 0;
        for (APawn* p = wi->PawnList; p && guard < 2000; p = p->NextPawn, guard++) {
            total++;
            if (p->ObjectFlags & 0x2000000000000000ull || p->bDeleteMe)
                continue;
            APlayerReplicationInfo* pri = p->PlayerReplicationInfo;
            if (p == pc->Pawn || (pri && !(pri->ObjectFlags & 0x2000000000000000ull) && !pri->bBot)) {
                players++;
                continue;
            }
            std::string name = p->ObjectArchetype ? p->ObjectArchetype->GetName() : p->Class->GetName();
            if (pri && pri->bBot)
                name += " (bot)";
            groups[name]++;
        }
        std::string line;
        for (auto& [name, n] : groups)
            line += (line.empty() ? "" : ", ") + std::to_string(n) + " " + name;

        std::string channels = "no server connection";
        uintptr_t world = reinterpret_cast<uintptr_t>(Globals::GetGWorld());
        uintptr_t netDriver = world ? *reinterpret_cast<uintptr_t*>(world + 0x128) : 0; // UWorld::NetDriver
        UNetConnection* conn = netDriver ? *reinterpret_cast<UNetConnection**>(netDriver + 0x80) : nullptr; // UNetDriver::ServerConnection
        if (conn) {
            int actorChannels = 0, noActor = 0;
            for (UChannel* ch : conn->Channels) {
                if (!ch || ch->Class != UActorChannel::StaticClass())
                    continue;
                actorChannels++;
                if (!reinterpret_cast<UActorChannel*>(ch)->Actor)
                    noActor++;
            }
            channels = "actor channels " + std::to_string(actorChannels) + ", " + std::to_string(noActor) + " with no actor | "
                + ServerNetworking::ClientPackageMapSummary();
        }

        char head[200];
        snprintf(head, sizeof head, "[MINIONS] %s: PawnList %i pawns (%i players'), %i others: ",
            Globals::amStandalone ? "solo" : "client", total, players, total - players);
        std::string out = std::string(head) + (line.empty() ? "none" : line) + " | " + channels;

        static std::string last;
        static ULONGLONG lastAt = 0;
        ULONGLONG now = GetTickCount64();
        if (out != last || now - lastAt >= 30000) {
            printf("%s\n", out.c_str());
            last = out;
            lastAt = now;
        }
    }

    void JoinServer(std::wstring ip) {
        std::wstring cmd = L"open ";

        cmd.append(ip);

        Engine::ExecConsoleCommand(cmd.c_str());
    }

    bool IsNetReady(UNetConnection* connection, int saturate) {
        return 1;
    }

    void ForceAlwaysNetReady() {
        static bool alreadyForced = false;

        if (!alreadyForced) {
            alreadyForced = true;
            static SafetyHookVmt vmts;
            static SafetyHookVm vms;

            vmts = safetyhook::create_vmt(SDKUtils::GetLastOfClass<UNetConnection>());

            vms = safetyhook::create_vm(vmts, 0x260 / 0x8, IsNetReady);
        }
    }
}
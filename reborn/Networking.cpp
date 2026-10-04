#include "Networking.hpp"

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

            averageTickrate = sum / Globals::Telemetry::Tickrates.size();

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

                if (ch && ch->Connection && ch->Actor) {
                    reinterpret_cast<void (*)(UActorChannel* channel)>(Globals::baseAddress + 0x0613050)(ch);
                    (*(reinterpret_cast<void(**)(UActorChannel*)>(*(__int64*)ch + 0x210)))(ch);
                }
            }

            // Globals::DisableGC = true;
        }
    }
}

namespace ClientNetworking {
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
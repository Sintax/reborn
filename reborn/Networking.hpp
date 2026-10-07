#pragma once

#include "json.hpp"
#include "safetyhook.hpp"

#include "Globals.hpp"
#include "Utils.hpp"
#include "ServerSettings.hpp"
#include "Engine.hpp"

namespace ServerNetworking {
    void GameControllerPoll();

    void InitListen();

    void LogConnectionPackageMap(UNetConnection* connection);

    bool PackageMapSupportsObject(UNetConnection* connection, UObject* object);

    bool PackageMapSupportsPackage(UNetConnection* connection, UObject* package);

    void RefreshServerPackageMaps(UNetConnection* connection);

    void SyncNewPawnSkin(APawn* pawn, const char* who);

    std::string ClientPackageMapSummary();

    bool ConnectionHasOnDemandPackage(UNetConnection* connection, UObject* package);

    void MarkOnDemandPackageLoaded(UNetConnection* connection, UObject* package);

    std::string OnDemandPackageListString(UNetConnection* connection);

    UActorChannel* GetActorChannelForActor(AActor* actor, UNetConnection* connection);

    bool IsLiveConnection(UNetConnection* connection);

    void ForgetClosedConnections(const char* where);

    bool IsChannelOnLiveConnection(UActorChannel* channel);

    std::vector<AActor*> BuildConsiderList(AWorldInfo* WorldInfo, UNetDriver* NetDriver);

    uint8_t GetConnectionState(UNetConnection* connection);

    bool CompareActorPriority(AActor* a, AActor* b);

    bool ConnectionFull(UNetConnection* connection);

    void TickNetServer(UTcpNetDriver* NetDriver);
}

namespace ClientNetworking {
    int LinkLoadedPendingPackages(UWorld* world, bool onlyNewest, const char* why);

    void JoinServer(std::wstring ip);

    bool IsNetReady(UNetConnection* connection, int saturate);

    void ForceAlwaysNetReady();
}
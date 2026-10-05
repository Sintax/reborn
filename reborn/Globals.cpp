#include "Globals.hpp"

namespace Globals {
    ServerPlayer* NextExpectedServerPlayer = nullptr;

    std::vector<ServerPlayer> ServerPlayers = std::vector<ServerPlayer>();

    uintptr_t baseAddress = 0x0;

    UTcpNetDriver* netDriver = 0x0;

    std::vector<UActorChannel*> channelsToClose = std::vector<UActorChannel*>();

    std::mutex mutex;

    float time = 0.0f;

    bool amServer = false;

    bool hasStartupMassacreHappened = false;

    float timeTillStartupMassacre = 0.0f;

    float timeTillHumanStart = 0.0f;

    bool haveHumansStarted = false;

    std::vector<int> AugStatus = std::vector<int>();

    bool hasDoneInitialTravel = false;

    std::vector<APoplarPlayerController*> ppcsWeSetupAugsFor = std::vector<APoplarPlayerController*>();

    __int64* methodsTable = nullptr;

    bool SaveManagerOpen = false;

    bool NewSaveOpen = false;

    bool SoloVSAIOpen = false;

    bool CampaignOpen = false;

    bool OperationsOpen = false;

    bool amStandalone = false;

    bool DirectConnectOpen = false;

    bool ServerBrowserOpen = false;

    bool CreateGameOpen = false;

    bool didStandaloneCharacterInitialization = false;

    float timeTillMutationInit = 0.0f;

    float ShutdownTimer = 0.0f;

    std::vector<Metagame::SaveFile> saveFiles = std::vector<Metagame::SaveFile>();

    unsigned int CurrentSaveFile = 0;

    bool didSendPreferencesToServer = false;

    std::string selectedCharacter = "";

    Metagame::Item* GearSlotOne = nullptr;
    Metagame::Item* GearSlotTwo = nullptr;
    Metagame::Item* GearSlotThree = nullptr;

    Metagame::CharacterSkin* CharacterSkin = nullptr;
    Metagame::CharacterTaunt* CharacterTaunt = nullptr;

    float timeSinceGameControllerServerPoll = 0.0f;

    std::shared_ptr<httplib::Client> GameCoordinatorHttpClient;

    std::vector<GameCoordinator::Lobby> Lobbies = std::vector<GameCoordinator::Lobby>();

    ELaunchSequenceState LaunchSequenceState = ELaunchSequenceState::NotOpen;

    const wchar_t* LaunchCommand = nullptr;

    unsigned int MatchIndex = 0;

    std::vector<GameCoordinator::ServerBrowserEntry> ServerBrowserEntries;
    GameCoordinator::ServerBrowserEntry CurrentMatchEntry;

    bool ConnectedToGameCoordinatorMatch = false;

    bool DisplayWaitingForPlayers = false;

    std::optional<GameCoordinator::Lobby> CurrentLobby = std::optional<GameCoordinator::Lobby>();

    std::mutex NetworkObjectListMutex = std::mutex();

    std::vector<AActor*> NetworkObjectList = std::vector<AActor*>();

    bool CharacterSelectMenuOpen = false;

    bool CharacterSelectHasLockedIn = false;

    bool CharacterSelectThisPossesionsTheRealOne = false;

    bool GearViewerOpen = false;

    std::string MyGCGuid = "";
    std::string MyGCKey = "";

    UWorld* GetGWorld() {
        return *reinterpret_cast<UWorld**>(baseAddress + 0x34dfca0);
    }

    void OnWorldSwitch() {
        std::cout << "[GAME] Running world switch behaviors!" << std::endl;

        Globals::AugStatus.clear();
        Globals::ppcsWeSetupAugsFor.clear();
        Globals::hasStartupMassacreHappened = false;
        Globals::timeTillStartupMassacre = 0.0f;
        Globals::didStandaloneCharacterInitialization = false;
        Globals::didSendPreferencesToServer = false;
        Globals::CharacterSelectHasLockedIn = false;
        Globals::CharacterSelectThisPossesionsTheRealOne = false;

        // Actors outside any level (archetypes, class defaults) must not point at a world: a
        // rooted archetype with WorldInfo set keeps the old world alive and the map change after
        // it fails with "World ... not cleaned up by garbage collection!". Clear any that do.
        {
            UClass* actorClass = AActor::StaticClass();
            UClass* levelClass = ULevel::StaticClass();
            int cleared = 0;
            for (int i = 0; i < UObject::GObjObjects()->size(); i++) {
                UObject* obj = UObject::GObjObjects()->at(i);
                if (!obj || !obj->IsA(actorClass)) continue;
                AActor* actor = reinterpret_cast<AActor*>(obj);
                if (!actor->WorldInfo || (actor->Outer && actor->Outer->IsA(levelClass))) continue;
                std::cout << "[WORLDSWITCH] clearing WorldInfo " << actor->WorldInfo->GetFullName() << " from " << actor->GetFullName() << " (not in a level)" << std::endl;
                actor->WorldInfo = nullptr;
                cleared++;
            }
            std::cout << "[WORLDSWITCH] actors outside a level that pointed at a world: " << cleared << std::endl;
        }

        std::scoped_lock t(Globals::NetworkObjectListMutex);

        Globals::NetworkObjectList.clear();
    }

    namespace Telemetry {
        std::mutex TickrateMutex = std::mutex();

        std::vector<float> Tickrates = std::vector<float>();

        std::mutex ReplicationFNameMutex = std::mutex();

        std::vector<int> ReplicationFNames = std::vector<int>();
    }
}
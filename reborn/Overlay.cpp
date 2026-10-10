#include "Overlay.hpp"

#include "Globals.hpp"
#include "Engine.hpp"
#include "GameCoordinator.hpp"
#include "Constants.hpp"
#include "Hooks.hpp"
#include "LocalHost.hpp"

#include <algorithm>

namespace Overlay {
    void OpenServerBrowser() {
        std::thread t(GameCoordinator::RefreshServerBrowser);
        t.detach();

        Globals::ServerBrowserOpen = true;
    }

    void OpenDirectConnect() {
        Globals::DirectConnectOpen = true;
    }

    void UpdateWaitingForPlayers() {
        bool shouldExit = false;

        while (!shouldExit && !Globals::SaveManagerOpen) {
            GameCoordinator::RefreshWaitingForPlayers();

            if (Globals::CurrentMatchEntry.MatchStarted) {
                shouldExit = true;
                Globals::DisplayWaitingForPlayers = false;
            }
            else {
                Globals::DisplayWaitingForPlayers = true;
            }

            Sleep(2 * 1000);
        }
    }

    void StartLaunchSequence(const wchar_t* command) {
        if (Globals::amStandalone) {
            Globals::LaunchSequenceState = Globals::ELaunchSequenceState::CharacterSelect;
            Globals::GearSlotOne = nullptr;
            Globals::GearSlotTwo = nullptr;
            Globals::GearSlotThree = nullptr;
            Globals::CharacterSkin = nullptr;
            Globals::CharacterTaunt = nullptr;
            Globals::LaunchCommand = command;
        }
        else {
            Engine::ExecConsoleCommand(command);

            if (Globals::ConnectedToGameCoordinatorMatch) {
                Globals::ConnectedToGameCoordinatorMatch = false;

                std::thread t(UpdateWaitingForPlayers);
                t.detach();
            }
        }
    }

    void OpenSoloVSAI() {
        Globals::SoloVSAIOpen = true;
    }

    void OpenCampaign() {
        Globals::CampaignOpen = true;
    }

    void OpenOperations() {
        Globals::OperationsOpen = true;
    }

    void OpenSaveManager() {
        Globals::saveFiles = Metagame::ReadAllSaves();
        Globals::SaveManagerOpen = true;
    }

    std::string GetItemDisplayName(const Metagame::Item* item) {
        if (item) {
            return item->itemDisplayName;
        }

        return "No Item";
    }

    

    // Re-sends the lock-in 3 s later, on the game thread (it used to be a detached std::thread with
    // a Sleep). The controller is looked up again instead of being captured: the one that took the
    // first lock-in can be freed by then (the client swaps its placeholder controller for the
    // server's), and calling ProcessEvent on it from a foreign thread crashed that thread inside the
    // ProcessEvent hook while it held the hook's mutex, which froze the game thread for good.
    void UnfuckCharacterSelect(int characterSelectIDX) {
        APoplarPlayerController* ppc = SDKUtils::GetLocalPlayerController();
        if (!ppc) {
            printf("[GAME] character select re-send skipped: no live local player controller\n");
            return;
        }

        Globals::CharacterSelectThisPossesionsTheRealOne = true;

        ppc->ServerCharacterSelectInput(characterSelectIDX);

        ppc->ServerSetHasReceivedEntitlements();
        ppc->eventServerSelectCharacter(nullptr, nullptr, nullptr, true);
        ppc->ServerPlayerSelectClass(L"", L"");
    }

    void LockInCharacter(int i) {
        Globals::CharacterSelectHasLockedIn = true;

        APoplarPlayerController* ppc = SDKUtils::GetLocalPlayerController();
        if (!ppc) ppc = SDKUtils::GetLastOfClass<APoplarPlayerController>();
        if (!ppc) {
            printf("[GAME] lock in skipped: no player controller\n");
            return;
        }

        printf("[GAME] locking in character %i on %s\n", i, ppc->GetFullName().c_str());

        ppc->ServerCharacterSelectInput(i);

        ppc->ServerSetHasReceivedEntitlements();
        ppc->eventServerSelectCharacter(nullptr, nullptr, nullptr, true);
        ppc->ServerPlayerSelectClass(L"", L"");

        ppc->ServerCharacterSelectInput(i);

        Globals::selectedCharacter = Metagame::ReverseCharacterLookup(Constants::CharacterSelectCharacterTable[i]);

        Engine::RunOnGameThreadAfter(3.0f, [i] { UnfuckCharacterSelect(i); });
    }

    void Render() {
        if (Globals::LaunchSequenceState > Globals::ELaunchSequenceState::NotOpen) {
            ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f), ImGuiCond_Always);
            ImGui::Begin("Launch Sequence", &Globals::SoloVSAIOpen, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);

            ImGui::SetWindowFontScale(2.0f);

            ImVec2 textSize;
            if (Globals::LaunchSequenceState == Globals::ELaunchSequenceState::CharacterSelect) {
                textSize = ImGui::CalcTextSize("Select a Character");
            }
            else if (Globals::LaunchSequenceState == Globals::ELaunchSequenceState::GearSelect) {
                textSize = ImGui::CalcTextSize("Select your Gear & Cosmetics");
            }

            float windowWidth = ImGui::GetContentRegionAvail().x;

            float centerX = (windowWidth - textSize.x) * 0.5f;

            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + centerX);

            if (Globals::LaunchSequenceState == Globals::ELaunchSequenceState::CharacterSelect) {
                ImGui::Text("Select a Character");
            }
            else if (Globals::LaunchSequenceState == Globals::ELaunchSequenceState::GearSelect) {
                ImGui::Text("Select your Gear & Cosmetics");
            }

            if (Globals::LaunchSequenceState == Globals::ELaunchSequenceState::CharacterSelect) {
                for (int i = 0; i < Globals::saveFiles[Globals::CurrentSaveFile].characters.size(); i++) {
                    const Metagame::Character& character = Globals::saveFiles[Globals::CurrentSaveFile].characters[i];
                    if (ImGui::RadioButton((Constants::CharacterLookupTable.at(character.characterDisplayName) + " - Level " + std::to_string(character.level) + "/10").c_str(), character.characterDisplayName == Globals::selectedCharacter)) {
                        Globals::selectedCharacter = character.characterDisplayName;
                    };
                }
            }
            else if (Globals::LaunchSequenceState == Globals::ELaunchSequenceState::GearSelect) {
                ImGui::PushID("ItemSlotOne");

                static std::string displayOne = "No Item";

                displayOne = GetItemDisplayName(Globals::GearSlotOne);

                if (ImGui::BeginCombo("Gear Slot One", displayOne.c_str(), ImGuiComboFlags_WidthFitPreview | ImGuiComboFlags_HeightLarge)) {

                    static std::string displayOneFilter = "";

                    ImGui::InputText("Filter Gear", &displayOneFilter);

                    if (ImGui::Selectable("No Item", Globals::GearSlotOne == nullptr)) {
                        Globals::GearSlotOne = nullptr;
                    }

                    for (int i = 0; i < Globals::saveFiles[Globals::CurrentSaveFile].items.size(); i++) {
                        Metagame::Item& item = Globals::saveFiles[Globals::CurrentSaveFile].items[i];

                        if (displayOneFilter.empty() || item.itemDisplayName.contains(displayOneFilter)) {
                            if (ImGui::Selectable(item.itemDisplayName.c_str(), Globals::GearSlotOne == &item)) {
                                Globals::GearSlotOne = &item;
                            }
                            if (ImGui::IsItemHovered()) {
                                ImGui::BeginTooltip();
                                ImGui::Text(item.itemDisplayName.c_str());
                                ImGui::Separator();
                                ImGui::Text(item.itemFlavor.c_str());
                                ImGui::EndTooltip();
                            }
                        }
                    }

                    ImGui::EndCombo();
                }

                ImGui::PopID();

                ImGui::PushID("ItemSlotTwo");

                static std::string DisplayTwo = "No Item";

                DisplayTwo = GetItemDisplayName(Globals::GearSlotTwo);

                if (ImGui::BeginCombo("Gear Slot Two", DisplayTwo.c_str(), ImGuiComboFlags_WidthFitPreview | ImGuiComboFlags_HeightLarge)) {

                    static std::string displayTwoFilter = "";

                    ImGui::InputText("Filter Gear", &displayTwoFilter);

                    if (ImGui::Selectable("No Item", Globals::GearSlotTwo == nullptr)) {
                        Globals::GearSlotTwo = nullptr;
                    }

                    for (int i = 0; i < Globals::saveFiles[Globals::CurrentSaveFile].items.size(); i++) {
                        Metagame::Item& item = Globals::saveFiles[Globals::CurrentSaveFile].items[i];

                        if (displayTwoFilter.empty() || item.itemDisplayName.contains(displayTwoFilter)) {
                            if (ImGui::Selectable(item.itemDisplayName.c_str(), Globals::GearSlotTwo == &item)) {
                                Globals::GearSlotTwo = &item;
                            }
                            if (ImGui::IsItemHovered()) {
                                ImGui::BeginTooltip();
                                ImGui::Text(item.itemDisplayName.c_str());
                                ImGui::Separator();
                                ImGui::Text(item.itemFlavor.c_str());
                                ImGui::EndTooltip();
                            }
                        }
                    }

                    ImGui::EndCombo();
                }

                ImGui::PopID();

                ImGui::PushID("ItemSlotThree");

                static std::string DisplayThree = "No Item";

                DisplayThree = GetItemDisplayName(Globals::GearSlotThree);

                if (ImGui::BeginCombo("Gear Slot Three", DisplayThree.c_str(), ImGuiComboFlags_WidthFitPreview | ImGuiComboFlags_HeightLarge)) {
                    static std::string displayThreeFilter = "";

                    ImGui::InputText("Filter Gear", &displayThreeFilter);

                    if (ImGui::Selectable("No Item", Globals::GearSlotThree == nullptr)) {
                        Globals::GearSlotThree = nullptr;
                    }


                    for (int i = 0; i < Globals::saveFiles[Globals::CurrentSaveFile].items.size(); i++) {
                        Metagame::Item& item = Globals::saveFiles[Globals::CurrentSaveFile].items[i];

                        if (displayThreeFilter.empty() || item.itemDisplayName.contains(displayThreeFilter)) {
                            if (ImGui::Selectable(item.itemDisplayName.c_str(), Globals::GearSlotThree == &item)) {
                                Globals::GearSlotThree = &item;
                            }
                            if (ImGui::IsItemHovered()) {
                                ImGui::BeginTooltip();
                                ImGui::Text(item.itemDisplayName.c_str());
                                ImGui::Separator();
                                ImGui::Text(item.itemFlavor.c_str());
                                ImGui::EndTooltip();
                            }
                        }
                    }

                    ImGui::EndCombo();
                }

                ImGui::PopID();

                static std::string DisplaySkin = "Default Skin";

                if (Globals::CharacterSkin) {
                    DisplaySkin = Globals::CharacterSkin->skinDisplayName;
                }

                if (Globals::amStandalone) {
                    ImGui::PushID("SkinSelect");

                    if (ImGui::BeginCombo("Character Skin", DisplaySkin.c_str(), ImGuiComboFlags_WidthFitPreview | ImGuiComboFlags_HeightLarge)) {
                        static std::string skinFilter = "";

                        ImGui::InputText("Filter Skins", &skinFilter);

                        if (ImGui::Selectable("Default Skin", Globals::CharacterSkin == nullptr)) {
                            Globals::CharacterSkin = nullptr;
                        }

                        for (int i = 0; i < Globals::saveFiles[Globals::CurrentSaveFile].characterSkins.size(); i++) {
                            Metagame::CharacterSkin& skin = Globals::saveFiles[Globals::CurrentSaveFile].characterSkins[i];

                            if ((skinFilter.empty() || skin.skinDisplayName.contains(skinFilter)) && skin.characterName.contains(Globals::selectedCharacter)) {
                                if (ImGui::Selectable(skin.skinDisplayName.c_str(), Globals::CharacterSkin == &skin)) {
                                    Globals::CharacterSkin = &skin;
                                }
                            }
                        }

                        ImGui::EndCombo();
                    }

                    ImGui::PopID();
                }
            }

            float availWidth = ImGui::GetContentRegionAvail().x;
            float spacing = ImGui::GetStyle().ItemSpacing.x;
            float buttonWidth = (availWidth - spacing) * 0.5f;

            if (Globals::LaunchSequenceState == Globals::ELaunchSequenceState::CharacterSelect) {
                if (ImGui::Button("Close", ImVec2(buttonWidth, 0))) {
                    Globals::LaunchSequenceState = Globals::ELaunchSequenceState::NotOpen;
                }
            }
            else {
                if (ImGui::Button("Back", ImVec2(buttonWidth, 0))) {
                    Globals::LaunchSequenceState = (Globals::ELaunchSequenceState)(Globals::LaunchSequenceState - 1);
                }
            }

            ImGui::SameLine();

            if (Globals::LaunchSequenceState == Globals::ELaunchSequenceState::GearSelect) {
                if (ImGui::Button("Start!", ImVec2(buttonWidth, 0))) {
                    Globals::LaunchSequenceState = Globals::ELaunchSequenceState::NotOpen;
                    std::cout << "[GAME] Entering the match!" << std::endl;
                    Engine::ExecConsoleCommand(Globals::LaunchCommand);

                    if (!Globals::amStandalone && Globals::ConnectedToGameCoordinatorMatch) {
                        Globals::ConnectedToGameCoordinatorMatch = false;

                        std::thread t(UpdateWaitingForPlayers);
                        t.detach();
                    }
                }
            }
            else {
                if (ImGui::Button("Next", ImVec2(buttonWidth, 0))) {
                    Globals::LaunchSequenceState = (Globals::ELaunchSequenceState)(Globals::LaunchSequenceState + 1);
                }
            }

            ImGui::End();
        }

        if (Globals::CharacterSelectMenuOpen) {
            ImGui::Begin("Character & Gear Select", &Globals::CreateGameOpen, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar);

            ImGui::SetWindowFontScale(2.0f);

            if (ImGui::BeginTabBar("CharacterGearSelect")) {
                if (!Globals::CharacterSelectHasLockedIn) {
                    if (ImGui::BeginTabItem("Character Select")) {
                        for (int i = 0; i < 30; i++) {
                            if (ImGui::Button(("Lock In " + Constants::CharacterSelectCharacterTable[i]).c_str())) {
                                LockInCharacter(i);
                            }
                        }

                        ImGui::EndTabItem();
                    }
                }

                if (!Globals::selectedCharacter.empty()) {
                    if (ImGui::BeginTabItem("Gear Select")) {
                        ImGui::PushID("ItemSlotOne");

                        static std::string displayOne = "No Item";

                        displayOne = GetItemDisplayName(Globals::GearSlotOne);

                        if (ImGui::BeginCombo("Gear Slot One", displayOne.c_str(), ImGuiComboFlags_WidthFitPreview | ImGuiComboFlags_HeightLarge)) {

                            static std::string displayOneFilter = "";

                            ImGui::InputText("Filter Gear", &displayOneFilter);

                            if (ImGui::Selectable("No Item", Globals::GearSlotOne == nullptr)) {
                                Globals::GearSlotOne = nullptr;
                            }

                            for (int i = 0; i < Globals::saveFiles[Globals::CurrentSaveFile].items.size(); i++) {
                                Metagame::Item& item = Globals::saveFiles[Globals::CurrentSaveFile].items[i];

                                if (displayOneFilter.empty() || item.itemDisplayName.contains(displayOneFilter)) {
                                    if (ImGui::Selectable(item.itemDisplayName.c_str(), Globals::GearSlotOne == &item)) {
                                        Globals::GearSlotOne = &item;
                                    }
                                    if (ImGui::IsItemHovered()) {
                                        ImGui::BeginTooltip();
                                        ImGui::Text(item.itemDisplayName.c_str());
                                        ImGui::Separator();
                                        ImGui::Text(item.itemFlavor.c_str());
                                        ImGui::EndTooltip();
                                    }
                                }
                            }

                            ImGui::EndCombo();
                        }

                        ImGui::PopID();

                        ImGui::PushID("ItemSlotTwo");

                        static std::string DisplayTwo = "No Item";

                        DisplayTwo = GetItemDisplayName(Globals::GearSlotTwo);

                        if (ImGui::BeginCombo("Gear Slot Two", DisplayTwo.c_str(), ImGuiComboFlags_WidthFitPreview | ImGuiComboFlags_HeightLarge)) {

                            static std::string displayTwoFilter = "";

                            ImGui::InputText("Filter Gear", &displayTwoFilter);

                            if (ImGui::Selectable("No Item", Globals::GearSlotTwo == nullptr)) {
                                Globals::GearSlotTwo = nullptr;
                            }

                            for (int i = 0; i < Globals::saveFiles[Globals::CurrentSaveFile].items.size(); i++) {
                                Metagame::Item& item = Globals::saveFiles[Globals::CurrentSaveFile].items[i];

                                if (displayTwoFilter.empty() || item.itemDisplayName.contains(displayTwoFilter)) {
                                    if (ImGui::Selectable(item.itemDisplayName.c_str(), Globals::GearSlotTwo == &item)) {
                                        Globals::GearSlotTwo = &item;
                                    }
                                    if (ImGui::IsItemHovered()) {
                                        ImGui::BeginTooltip();
                                        ImGui::Text(item.itemDisplayName.c_str());
                                        ImGui::Separator();
                                        ImGui::Text(item.itemFlavor.c_str());
                                        ImGui::EndTooltip();
                                    }
                                }
                            }

                            ImGui::EndCombo();
                        }

                        ImGui::PopID();

                        ImGui::PushID("ItemSlotThree");

                        static std::string DisplayThree = "No Item";

                        DisplayThree = GetItemDisplayName(Globals::GearSlotThree);

                        if (ImGui::BeginCombo("Gear Slot Three", DisplayThree.c_str(), ImGuiComboFlags_WidthFitPreview | ImGuiComboFlags_HeightLarge)) {
                            static std::string displayThreeFilter = "";

                            ImGui::InputText("Filter Gear", &displayThreeFilter);

                            if (ImGui::Selectable("No Item", Globals::GearSlotThree == nullptr)) {
                                Globals::GearSlotThree = nullptr;
                            }


                            for (int i = 0; i < Globals::saveFiles[Globals::CurrentSaveFile].items.size(); i++) {
                                Metagame::Item& item = Globals::saveFiles[Globals::CurrentSaveFile].items[i];

                                if (displayThreeFilter.empty() || item.itemDisplayName.contains(displayThreeFilter)) {
                                    if (ImGui::Selectable(item.itemDisplayName.c_str(), Globals::GearSlotThree == &item)) {
                                        Globals::GearSlotThree = &item;
                                    }
                                    if (ImGui::IsItemHovered()) {
                                        ImGui::BeginTooltip();
                                        ImGui::Text(item.itemDisplayName.c_str());
                                        ImGui::Separator();
                                        ImGui::Text(item.itemFlavor.c_str());
                                        ImGui::EndTooltip();
                                    }
                                }
                            }

                            ImGui::EndCombo();
                        }

                        ImGui::PopID();

                        ImGui::EndTabItem();
                    }

                    if (ImGui::BeginTabItem("Cosmetic Select")) {
                        static std::string DisplayTaunt = "Default Taunt";

                        if (Globals::CharacterTaunt) {
                            DisplayTaunt = Globals::CharacterTaunt->tauntDisplayName;
                        }

                        ImGui::PushID("TauntSelect");

                        if (ImGui::BeginCombo("Character Taunt", DisplayTaunt.c_str(), ImGuiComboFlags_WidthFitPreview | ImGuiComboFlags_HeightLarge)) {
                            static std::string tauntFilter = "";

                            ImGui::InputText("Filter Taunts", &tauntFilter);

                            if (ImGui::Selectable("Default Taunt", Globals::CharacterTaunt == nullptr)) {
                                Globals::CharacterTaunt = nullptr;
                            }

                            for (int i = 0; i < Globals::saveFiles[Globals::CurrentSaveFile].characterTaunts.size(); i++) {
                                Metagame::CharacterTaunt& taunt = Globals::saveFiles[Globals::CurrentSaveFile].characterTaunts[i];

                                if ((tauntFilter.empty() || taunt.tauntDisplayName.contains(tauntFilter)) && taunt.characterName.contains(Globals::selectedCharacter)) {
                                    if (ImGui::Selectable(taunt.tauntDisplayName.c_str(), Globals::CharacterTaunt == &taunt)) {
                                        Globals::CharacterTaunt = &taunt;

                                        // We're on the client here, so we should only ever have one PPC (aside from the CDO), so this *shouldn't* break. TODO refactor tho
                                        APoplarPlayerController* ppc = SDKUtils::GetLastOfClass<APoplarPlayerController>();

                                        ppc->eventServerSelectCharacterTaunt(UObject::FindObject<UPoplarMetaTauntDefinition>(taunt.tauntObjectName));
                                    }
                                }
                            }

                            ImGui::EndCombo();
                        }

                        ImGui::PopID();

                        static std::string DisplaySkin = "Default Skin";

                        if (Globals::CharacterSkin) {
                            DisplaySkin = Globals::CharacterSkin->skinDisplayName;
                        }

                        ImGui::PushID("SkinSelect");

                        if (ImGui::BeginCombo("Character Skin", DisplaySkin.c_str(), ImGuiComboFlags_WidthFitPreview | ImGuiComboFlags_HeightLarge)) {
                            static std::string skinFilter = "";

                            ImGui::InputText("Filter Skins", &skinFilter);

                            if (ImGui::Selectable("Default Skin", Globals::CharacterSkin == nullptr)) {
                                Globals::CharacterSkin = nullptr;
                            }

                            for (int i = 0; i < Globals::saveFiles[Globals::CurrentSaveFile].characterSkins.size(); i++) {
                                Metagame::CharacterSkin& skin = Globals::saveFiles[Globals::CurrentSaveFile].characterSkins[i];

                                if ((skinFilter.empty() || skin.skinDisplayName.contains(skinFilter)) && skin.characterName.contains(Globals::selectedCharacter)) {
                                    if (ImGui::Selectable(skin.skinDisplayName.c_str(), Globals::CharacterSkin == &skin)) {
                                        Globals::CharacterSkin = &skin;

                                        // We're on the client here, so we should only ever have one PPC (aside from the CDO), so this *shouldn't* break. TODO refactor tho
                                        APoplarPlayerController* ppc = SDKUtils::GetLastOfClass<APoplarPlayerController>();

                                        ppc->eventServerSelectCharacterSkin(UObject::FindObject<UPoplarMetaSkinDefinition>(skin.skinObjectName));
                                    }
                                }
                            }

                            ImGui::EndCombo();
                        }

                        ImGui::PopID();

                        ImGui::EndTabItem();
                    }
                }

                ImGui::EndTabBar();
            }

            ImGui::End();
        }

        // The server started by "Host Game" is ready: join it like a Direct Connect to this PC.
        if (LocalHost::TakeReadyToJoin()) {
            Globals::amStandalone = false;
            Globals::ConnectedToGameCoordinatorMatch = false;
            StartLaunchSequence(L"open 127.0.0.1");
        }

        LocalHost::Status hostStatus = LocalHost::GetStatus();
        if (hostStatus == LocalHost::Status::Starting || hostStatus == LocalHost::Status::Failed) {
            ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x * 0.4f, 0), ImGuiCond_Always);
            ImGui::Begin("Hosting", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::SetWindowFontScale(2.0f);
            ImGui::TextWrapped("%s", LocalHost::GetStatusText().c_str());
            if (hostStatus == LocalHost::Status::Starting) {
                if (ImGui::Button("Cancel", ImVec2(-1.0f, 0))) LocalHost::Stop();
            }
            else if (ImGui::Button("Close", ImVec2(-1.0f, 0))) {
                LocalHost::Dismiss();
            }
            ImGui::End();
        }

        if (Globals::CreateGameOpen) {
            static std::pair<std::string, std::string> MapMode = std::make_pair("Story - The Algorithm", "open Caverns_P");

            static int MaxNumPlayers = 1;

            ImGui::Begin("Host a Game", &Globals::CreateGameOpen, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

            ImGui::SetWindowFontScale(2.0f);

            ImGui::PushID("MapMode");

            static std::string mapModeDisplay = "Story - The Algorithm";

            mapModeDisplay = MapMode.first;

            if (ImGui::BeginCombo("Map/Mode Selection", mapModeDisplay.c_str(), ImGuiComboFlags_WidthFitPreview | ImGuiComboFlags_HeightLarge)) {
                static std::string displayOneFilter = "";

                ImGui::InputText("Filter Maps/Modes", &displayOneFilter);

                for (int i = 0; i < Constants::GameCreationOptions.size(); i++) {
                    auto& cmpMapMode = Constants::GameCreationOptions[i];

                    if (displayOneFilter.empty() || cmpMapMode.first.contains(displayOneFilter)) {
                        if (ImGui::Selectable(cmpMapMode.first.c_str(), MapMode == cmpMapMode)) {
                            MapMode = cmpMapMode;
                        }
                    }
                }

                ImGui::EndCombo();
            }

            ImGui::PopID();

            // Story missions and operations take up to 5 players; versus up to 10.
            const bool pve = MapMode.first.starts_with("Story") || MapMode.first.starts_with("Operations");
            const int maxPlayers = pve ? 5 : 10;
            MaxNumPlayers = std::clamp(MaxNumPlayers, 1, maxPlayers);
            ImGui::SliderInt("Players (including you)", &MaxNumPlayers, 1, maxPlayers);

            ImGui::TextWrapped("The match starts when this many players have joined. Friends join with Direct Connect to your PC's address (UDP port 7777).");

            const bool busy = LocalHost::GetStatus() == LocalHost::Status::Starting;
            if (busy) ImGui::BeginDisabled();
            if (ImGui::Button("Host Game!", ImVec2(-1.0f, 0))) {
                Globals::CreateGameOpen = false;
                std::string map = MapMode.second.starts_with("open ") ? MapMode.second.substr(5) : MapMode.second;
                LocalHost::Start(map, MapMode.first, MaxNumPlayers);
            }
            if (busy) ImGui::EndDisabled();
            ImGui::End();
        }

        if (Globals::DisplayWaitingForPlayers) {
            ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x * 0.25f, ImGui::GetIO().DisplaySize.y * 0.25f), ImGuiCond_Always);

            ImGui::Begin("Waiting For Players", &Globals::DisplayWaitingForPlayers, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);

            ImGui::SetWindowFontScale(2.0f);

            std::string waitingString = "";

            if (Globals::CurrentMatchEntry.CurrentNumPlayers == Globals::CurrentMatchEntry.MaxNumPlayers) {
                waitingString += "All Players Connected!";
            }
            else if (Globals::CurrentMatchEntry.CurrentNumPlayers > Globals::CurrentMatchEntry.MaxNumPlayers) {
                waitingString += "You're in for a good one!";
            }
            else {
                waitingString += "Waiting For Players (";
                waitingString += std::to_string(Globals::CurrentMatchEntry.CurrentNumPlayers);
                waitingString += "/";
                waitingString += std::to_string(Globals::CurrentMatchEntry.MaxNumPlayers);
                waitingString += ")...";
            }

            ImVec2 textSize;
            textSize = ImGui::CalcTextSize(waitingString.c_str());

            float windowWidth = ImGui::GetContentRegionAvail().x;

            float centerX = (windowWidth - textSize.x) * 0.5f;

            float windowHeight = ImGui::GetContentRegionAvail().y;

            float centerY = (windowHeight - textSize.y) * 0.5f;

            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + centerX);

            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + centerY);

            ImGui::Text(waitingString.c_str());

            ImGui::End();
        }

        if (Globals::ServerBrowserOpen) {
            ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f), ImGuiCond_Always);
            ImGui::Begin("Server Browser", &Globals::ServerBrowserOpen, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);

            ImGui::SetWindowFontScale(2.0f);

            ImVec2 textSize;
            textSize = ImGui::CalcTextSize("Server Browser");

            float windowWidth = ImGui::GetContentRegionAvail().x;

            float centerX = (windowWidth - textSize.x) * 0.5f;

            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + centerX);

            ImGui::Text("Server Browser");

            float availWidth = ImGui::GetContentRegionAvail().x;
            float spacing = ImGui::GetStyle().ItemSpacing.x;
            float buttonWidth = (availWidth - spacing) * 0.25f;

            if (ImGui::Button("Host Game", ImVec2(buttonWidth, 0))) {
                Globals::ServerBrowserOpen = false;
                Globals::CreateGameOpen = true;
            }

            ImGui::SameLine();

            if (ImGui::Button("Direct Connect", ImVec2(buttonWidth, 0))) {
                Globals::ServerBrowserOpen = false;
                Globals::DirectConnectOpen = true;
            }

            ImGui::SameLine();

            if (ImGui::Button("Refresh Server Browser", ImVec2(buttonWidth, 0))) {
                std::thread t(GameCoordinator::RefreshServerBrowser);
                t.detach();
            }

            ImGui::SameLine();

            if (ImGui::Button("Close", ImVec2(buttonWidth, 0))) {
                Globals::ServerBrowserOpen = false;
            }

            if (ImGui::BeginTable("ServerBrowserTable", 5,
                ImGuiTableFlags_Borders |
                ImGuiTableFlags_RowBg |
                ImGuiTableFlags_Resizable |
                ImGuiTableFlags_Sortable |
                ImGuiTableFlags_ScrollY)) {

                // Setup table headers
                ImGui::TableSetupColumn("Server Name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Map & Mode", ImGuiTableColumnFlags_WidthFixed, 150.0f);
                ImGui::TableSetupColumn("Players", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();

                for (size_t i = 0; i < Globals::ServerBrowserEntries.size(); ++i) {
                    const auto& server = Globals::ServerBrowserEntries[i];
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(server.InstanceName.c_str());

                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(server.HumanReadableInstanceMapMode.c_str());

                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%d/%d", server.CurrentNumPlayers, server.MaxNumPlayers);

                    ImGui::TableSetColumnIndex(3);
                    if (server.MatchStarted) {
                        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), "In Progress");
                    }
                    else {
                        ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.0f, 1.0f), "Joinable!");
                    }

                    ImGui::TableSetColumnIndex(4);

                    ImGui::PushID(static_cast<int>(i));

                    bool canJoin = server.CurrentNumPlayers < server.MaxNumPlayers && !server.MatchStarted;
                    if (!canJoin) {
                        ImGui::BeginDisabled();
                    }

                    if (ImGui::Button("Join")) {
                        Globals::amStandalone = false;
                        Globals::ServerBrowserOpen = false;

                        Globals::MatchIndex = i;
                        Globals::CurrentMatchEntry = server;   // the waiting screen follows this game by its address
                        Globals::ConnectedToGameCoordinatorMatch = true;

                        std::wstring wLaunchCommand = std::wstring(server.ServerConnectString.begin(), server.ServerConnectString.end());

                        StartLaunchSequence(wcsdup(wLaunchCommand.c_str()));
                    }

                    if (!canJoin) {
                        ImGui::EndDisabled();
                    }

                    ImGui::PopID();
                }

                ImGui::EndTable();
            }

            ImGui::End();
        }

        if (Globals::DirectConnectOpen) {
            ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x * 0.25f, ImGui::GetIO().DisplaySize.y * 0.25f), ImGuiCond_Always);
            ImGui::Begin("Direct Connect", &Globals::DirectConnectOpen, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);

            ImGui::SetWindowFontScale(2.0f);

            ImVec2 textSize;
            textSize = ImGui::CalcTextSize("Direct Connect");

            float windowWidth = ImGui::GetContentRegionAvail().x;

            float centerX = (windowWidth - textSize.x) * 0.5f;

            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + centerX);

            ImGui::Text("Direct Connect");

            static std::string ipToConnectTo = "";

            ImGui::InputText("Enter Server IP: ", &ipToConnectTo);

            float availWidth = ImGui::GetContentRegionAvail().x;
            float spacing = ImGui::GetStyle().ItemSpacing.x;
            float buttonWidth = (availWidth - spacing) * 0.5f;

            if (ImGui::Button("Close", ImVec2(buttonWidth, 0))) {
                Globals::DirectConnectOpen = false;
            }

            ImGui::SameLine();

            if (!ipToConnectTo.empty()) {
                if (ImGui::Button("Start!", ImVec2(buttonWidth, 0))) {
                    std::wstring wIp(ipToConnectTo.begin(), ipToConnectTo.end());

                    std::wstring wcmd = L"open ";

                    Globals::amStandalone = false;

                    Globals::ConnectedToGameCoordinatorMatch = false;

                    StartLaunchSequence(wcsdup((wcmd.append(wIp).c_str())));

                    Globals::DirectConnectOpen = false;
                }
            }

            ImGui::End();
        }

        if (Globals::SaveManagerOpen) {
            static int selectedSave = 0;

            ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f), ImGuiCond_Always);
            ImGui::Begin("Reborn Save Manager", &Globals::SaveManagerOpen, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
            ImGui::SetWindowFontScale(2.0f);

            if (ImGui::Button("+ New Save", ImVec2(-1.0f, 0))) {
                Globals::NewSaveOpen = true;
            }
            for (int i = 0; i < Globals::saveFiles.size(); i++) {
                if (ImGui::RadioButton(Globals::saveFiles[i].name.c_str(), selectedSave == i)) {
                    selectedSave = i;
                }
            }
            if (Globals::saveFiles.size() > 0) {
                if (ImGui::Button("Select Save & Start Game!", ImVec2(-1.0f, 0))) {
                    Globals::SaveManagerOpen = false;
                    Globals::CurrentSaveFile = selectedSave;
                    Hooks::StartupCompletedHook();
                }
            }
            else {
                ImGui::Text("No saves found, create one!");
            }
            ImGui::End();
        }

        if (Globals::SoloVSAIOpen) {
            static const wchar_t* commandToExecute = nullptr;

            ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f), ImGuiCond_Always);
            ImGui::Begin("Solo VS AI", &Globals::SoloVSAIOpen, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);

            ImGui::SetWindowFontScale(2.0f);

            ImVec2 textSize = ImGui::CalcTextSize("Versus Private (Solo vs AI)");

            float windowWidth = ImGui::GetContentRegionAvail().x;

            float centerX = (windowWidth - textSize.x) * 0.5f;

            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + centerX);

            ImGui::Text("Versus Private (Solo vs AI)");

            if (ImGui::CollapsingHeader("Supercharge")) {
                ImGui::Indent();
                for (int i = 0; i < Constants::PvAIMaps::Supercharge.size(); i++) {
                    if (ImGui::RadioButton(Constants::PvAIMaps::Supercharge[i].first.c_str(), commandToExecute == Constants::PvAIMaps::Supercharge[i].second)) {
                        commandToExecute = Constants::PvAIMaps::Supercharge[i].second;
                    }
                }
                ImGui::Unindent();
            }

            if (ImGui::CollapsingHeader("Incursion")) {
                ImGui::Indent();
                for (int i = 0; i < Constants::PvAIMaps::Incursion.size(); i++) {
                    if (ImGui::RadioButton(Constants::PvAIMaps::Incursion[i].first.c_str(), commandToExecute == Constants::PvAIMaps::Incursion[i].second)) {
                        commandToExecute = Constants::PvAIMaps::Incursion[i].second;
                    }
                }
                ImGui::Unindent();
            }

            if (ImGui::CollapsingHeader("Capture")) {
                ImGui::Indent();
                for (int i = 0; i < Constants::PvAIMaps::Capture.size(); i++) {
                    if (ImGui::RadioButton(Constants::PvAIMaps::Capture[i].first.c_str(), commandToExecute == Constants::PvAIMaps::Capture[i].second)) {
                        commandToExecute = Constants::PvAIMaps::Capture[i].second;
                    }
                }
                ImGui::Unindent();
            }

            if (ImGui::CollapsingHeader("Meltdown")) {
                ImGui::Indent();
                for (int i = 0; i < Constants::PvAIMaps::Meltdown.size(); i++) {
                    if (ImGui::RadioButton(Constants::PvAIMaps::Meltdown[i].first.c_str(), commandToExecute == Constants::PvAIMaps::Meltdown[i].second)) {
                        commandToExecute = Constants::PvAIMaps::Meltdown[i].second;
                    }
                }
                ImGui::Unindent();
            }

            if (ImGui::CollapsingHeader("Face-Off")) {
                ImGui::Indent();
                for (int i = 0; i < Constants::PvAIMaps::FaceOff.size(); i++) {
                    if (ImGui::RadioButton(Constants::PvAIMaps::FaceOff[i].first.c_str(), commandToExecute == Constants::PvAIMaps::FaceOff[i].second)) {
                        commandToExecute = Constants::PvAIMaps::FaceOff[i].second;
                    }
                }
                ImGui::Unindent();
            }

            float availWidth = ImGui::GetContentRegionAvail().x;
            float spacing = ImGui::GetStyle().ItemSpacing.x;
            float buttonWidth = (availWidth - spacing) * 0.5f;

            if (ImGui::Button("Close", ImVec2(buttonWidth, 0))) {
                Globals::SoloVSAIOpen = false;
            }

            if (commandToExecute != nullptr) {
                ImGui::SameLine();

                if (ImGui::Button("Play!", ImVec2(buttonWidth, 0))) {
                    Globals::amStandalone = true;
                    StartLaunchSequence(commandToExecute);
                    commandToExecute = nullptr;
                    Globals::SoloVSAIOpen = false;
                }
            }



            ImGui::End();
        }

        if (Globals::CampaignOpen) {
            static const wchar_t* commandToExecute = nullptr;

            ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f), ImGuiCond_Always);
            ImGui::Begin("Campaign", &Globals::SoloVSAIOpen, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);

            ImGui::SetWindowFontScale(2.0f);

            ImVec2 textSize = ImGui::CalcTextSize("Campaign (Story Private)");

            float windowWidth = ImGui::GetContentRegionAvail().x;

            float centerX = (windowWidth - textSize.x) * 0.5f;

            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + centerX);

            ImGui::Text("Campaign (Story Private)");

            for (int i = 0; i < Constants::CampaignMaps.size(); i++) {
                if (ImGui::RadioButton(Constants::CampaignMaps[i].first.c_str(), commandToExecute == Constants::CampaignMaps[i].second)) {
                    commandToExecute = Constants::CampaignMaps[i].second;
                }
            }

            float availWidth = ImGui::GetContentRegionAvail().x;
            float spacing = ImGui::GetStyle().ItemSpacing.x;
            float buttonWidth = (availWidth - spacing) * 0.5f;

            if (ImGui::Button("Close", ImVec2(buttonWidth, 0))) {
                Globals::CampaignOpen = false;
            }

            if (commandToExecute != nullptr) {
                ImGui::SameLine();

                if (ImGui::Button("Play!", ImVec2(buttonWidth, 0))) {
                    Globals::amStandalone = true;
                    StartLaunchSequence(commandToExecute);
                    commandToExecute = nullptr;
                    Globals::CampaignOpen = false;
                }
            }

            ImGui::End();
        }

        if (Globals::OperationsOpen) {
            static const wchar_t* commandToExecute = nullptr;

            ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f), ImGuiCond_Always);
            ImGui::Begin("Operations", &Globals::SoloVSAIOpen, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);

            ImGui::SetWindowFontScale(2.0f);

            ImVec2 textSize = ImGui::CalcTextSize("Operations (Operations Private)");

            float windowWidth = ImGui::GetContentRegionAvail().x;

            float centerX = (windowWidth - textSize.x) * 0.5f;

            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + centerX);

            ImGui::Text("Operations (Operations Private)");

            for (int i = 0; i < Constants::OperationMaps.size(); i++) {
                if (ImGui::RadioButton(Constants::OperationMaps[i].first.c_str(), commandToExecute == Constants::OperationMaps[i].second)) {
                    commandToExecute = Constants::OperationMaps[i].second;
                }
            }

            float availWidth = ImGui::GetContentRegionAvail().x;
            float spacing = ImGui::GetStyle().ItemSpacing.x;
            float buttonWidth = (availWidth - spacing) * 0.5f;

            if (ImGui::Button("Close", ImVec2(buttonWidth, 0))) {
                Globals::OperationsOpen = false;
            }

            if (commandToExecute != nullptr) {
                ImGui::SameLine();

                if (ImGui::Button("Play!", ImVec2(buttonWidth, 0))) {
                    Globals::amStandalone = true;
                    StartLaunchSequence(commandToExecute);
                    commandToExecute = nullptr;
                    Globals::OperationsOpen = false;
                }
            }

            ImGui::End();
        }

        if (Globals::NewSaveOpen) {
            static std::string saveName = "";
            static bool startWithEverything = true;

            ImGui::Begin("New Save", &Globals::NewSaveOpen, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

            ImGui::SetWindowFontScale(2.0f);

            ImGui::InputText("Display Name (Visible in Multiplayer & Singleplayer)", &saveName);
            /*
            if (ImGui::RadioButton("Earn Gear, Mutations & Character Skins by Playing!", !startWithEverything)) {
                startWithEverything = false;
            }
            ImGui::SameLine();
            */
            if (ImGui::RadioButton("Start With Everything!", startWithEverything)) {
                startWithEverything = true;
            }
            if (ImGui::Button("Create Save File!", ImVec2(-1.0f, 0))) {
                Globals::NewSaveOpen = false;
                Metagame::CreateNewSave(saveName, startWithEverything);
            }
            ImGui::End();
        }
    }
}
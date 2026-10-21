#include "Overlay.hpp"

#include "Access.hpp"
#include "Globals.hpp"
#include "Engine.hpp"
#include "GameCoordinator.hpp"
#include "Constants.hpp"
#include "Hooks.hpp"
#include "LocalHost.hpp"
#include "StartKey.hpp"
#include "StartGameWindow.hpp"
#include "UiStyle.hpp"

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
            // The game's own select screen is the waiting room now; nothing is drawn while it waits.
            Engine::ExecConsoleCommand(command);
            Globals::ConnectedToGameCoordinatorMatch = false;
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

    

    // One gear slot: a drop-down of the save's items with a filter box.
    void GearCombo(const char* id, Metagame::Item*& slot) {
        ImGui::PushID(id);
        std::string shown = GetItemDisplayName(slot);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##slot", shown.c_str(), ImGuiComboFlags_HeightLarge)) {
            static std::string filter;
            ImGui::InputText("Filter Gear", &filter);
            if (ImGui::Selectable("No Item", slot == nullptr)) slot = nullptr;
            for (Metagame::Item& item : Globals::saveFiles[Globals::CurrentSaveFile].items) {
                if (!filter.empty() && !item.itemDisplayName.contains(filter)) continue;
                if (ImGui::Selectable(item.itemDisplayName.c_str(), slot == &item)) slot = &item;
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

    void Render() {
        UiStyle::Apply();

        if (Globals::LaunchSequenceState > Globals::ELaunchSequenceState::NotOpen) {
            UiStyle::NextWindow(0.5f);
            ImGui::Begin("Launch Sequence", &Globals::SoloVSAIOpen, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);

            UiStyle::WindowFont();

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

            float buttonWidth = UiStyle::ButtonWidth(2);

            if (Globals::LaunchSequenceState == Globals::ELaunchSequenceState::CharacterSelect) {
                if (UiStyle::Button("Close", buttonWidth)) {
                    Globals::LaunchSequenceState = Globals::ELaunchSequenceState::NotOpen;
                }
            }
            else {
                if (UiStyle::Button("Back", buttonWidth)) {
                    Globals::LaunchSequenceState = (Globals::ELaunchSequenceState)(Globals::LaunchSequenceState - 1);
                }
            }

            ImGui::SameLine();

            if (Globals::LaunchSequenceState == Globals::ELaunchSequenceState::GearSelect) {
                if (UiStyle::Button("Start!", buttonWidth)) {
                    Globals::LaunchSequenceState = Globals::ELaunchSequenceState::NotOpen;
                    std::cout << "[GAME] Entering the match!" << std::endl;
                    Engine::ExecConsoleCommand(Globals::LaunchCommand);
                }
            }
            else {
                if (UiStyle::Button("Next", buttonWidth)) {
                    Globals::LaunchSequenceState = (Globals::ELaunchSequenceState)(Globals::LaunchSequenceState + 1);
                }
            }

            ImGui::End();
        }

        // The Gear box, while the game's own hero screen is up. Bottom-right: in Versus the
        // top-right holds the enemy team list, the left our team list, the centre the hero.
        // The screen's own loadout row cannot replace it yet: loadouts came from the old online
        // service, so every player has none (NumLoadoutEntitlements 0) and the cells stay locked.
        if (Globals::GearBoxOpen && Globals::CurrentSaveFile < Globals::saveFiles.size()) {
            ImVec2 screen = ImGui::GetIO().DisplaySize;
            ImGui::SetNextWindowPos(ImVec2(screen.x - 16.0f, screen.y - 16.0f), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
            ImGui::SetNextWindowSize(ImVec2(screen.x * 0.22f, 0.0f), ImGuiCond_Always);
            ImGui::Begin("Gear", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
            UiStyle::WindowFont();
            ImGui::TextUnformatted("Gear");
            GearCombo("GearOne", Globals::GearSlotOne);
            GearCombo("GearTwo", Globals::GearSlotTwo);
            GearCombo("GearThree", Globals::GearSlotThree);
            ImGui::End();
        }

        // "Start a Game": runs every frame so a start under way is followed even with the window closed.
        static StartGameModel startGame;
        StartGameWindow::Draw(startGame);

        if (Globals::ServerBrowserOpen) {
            UiStyle::NextWindow(0.5f);
            ImGui::Begin("Server Browser", &Globals::ServerBrowserOpen, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);

            UiStyle::WindowFont();

            ImVec2 textSize;
            textSize = ImGui::CalcTextSize("Server Browser");

            float windowWidth = ImGui::GetContentRegionAvail().x;

            float centerX = (windowWidth - textSize.x) * 0.5f;

            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + centerX);

            ImGui::Text("Server Browser");

            // Two buttons a row, so the labels fit at small window sizes.
            float buttonWidth = UiStyle::ButtonWidth(2);

            if (UiStyle::Button("Start a Game", buttonWidth)) {
                Globals::ServerBrowserOpen = false;
                // Starting on the server names the game after the player, so ask for a name first.
                if (!StartKey::Key().empty() && StartKey::PlayerName().empty()) Globals::PlayerNameOpen = true;
                else Globals::StartGameOpen = true;
            }

            ImGui::SameLine();

            if (UiStyle::Button("Direct Connect", buttonWidth)) {
                Globals::ServerBrowserOpen = false;
                Globals::DirectConnectOpen = true;
            }

            if (UiStyle::Button("Refresh", buttonWidth)) {
                std::thread t(GameCoordinator::RefreshServerBrowser);
                t.detach();
            }

            ImGui::SameLine();

            if (UiStyle::Button("Close", buttonWidth)) {
                Globals::ServerBrowserOpen = false;
            }

            if (ImGui::BeginTable("ServerBrowserTable", 5,
                ImGuiTableFlags_Borders |
                ImGuiTableFlags_RowBg |
                ImGuiTableFlags_Resizable |
                ImGuiTableFlags_Sortable |
                ImGuiTableFlags_ScrollY)) {

                // Setup table headers; the fixed columns grow with the text size
                const float colScale = UiStyle::FontScale(ImGui::GetIO().DisplaySize.y);
                ImGui::TableSetupColumn("Server Name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Map & Mode", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Players", ImGuiTableColumnFlags_WidthFixed, 80.0f * colScale);
                ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 100.0f * colScale);
                ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 80.0f * colScale);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();

                const auto browserEntries = GameCoordinator::BrowserEntriesSnapshot();
                for (size_t i = 0; i < browserEntries.size(); ++i) {
                    const auto& server = browserEntries[i];
                    if (server.MatchStarted && !server.AllowLateJoin) continue;
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

                    bool canJoin = server.CurrentNumPlayers < server.MaxNumPlayers && (!server.MatchStarted || server.AllowLateJoin);
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

        if (Globals::PlayerNameOpen) {
            static std::string name = StartKey::PlayerName();
            UiStyle::NextWindow();
            ImGui::Begin("Your name", &Globals::PlayerNameOpen, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
            UiStyle::WindowFont();
            ImGui::TextWrapped("Shown to other players and used to name your games.");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##name", "Shown to other players", &name);
            std::string trimmed = StartKey::TrimKey(name);
            const bool okEnabled = !trimmed.empty() && trimmed.size() <= 24;
            if (!okEnabled) ImGui::BeginDisabled();
            if (UiStyle::Button("OK")) {
                StartKey::SavePlayerName(trimmed);
                Globals::PlayerNameOpen = false;
                Globals::StartGameOpen = true;
            }
            if (!okEnabled) ImGui::EndDisabled();
            ImGui::End();
        }

        if (Globals::DirectConnectOpen) {
            UiStyle::NextWindow();
            ImGui::Begin("Direct Connect", &Globals::DirectConnectOpen, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);

            UiStyle::WindowFont();

            ImVec2 textSize;
            textSize = ImGui::CalcTextSize("Direct Connect");

            float windowWidth = ImGui::GetContentRegionAvail().x;

            float centerX = (windowWidth - textSize.x) * 0.5f;

            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + centerX);

            ImGui::Text("Direct Connect");

            static std::string ipToConnectTo = "";

            ImGui::InputText("Enter Server IP: ", &ipToConnectTo);

            float buttonWidth = UiStyle::ButtonWidth(2);

            if (UiStyle::Button("Close", buttonWidth)) {
                Globals::DirectConnectOpen = false;
            }

            ImGui::SameLine();

            if (!ipToConnectTo.empty()) {
                if (UiStyle::Button("Start!", buttonWidth)) {
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

            UiStyle::NextWindow(0.5f);
            ImGui::Begin("Reborn Save Manager", &Globals::SaveManagerOpen, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
            UiStyle::WindowFont();

            if (UiStyle::Button("+ New Save")) {
                Globals::NewSaveOpen = true;
            }
            for (int i = 0; i < Globals::saveFiles.size(); i++) {
                if (ImGui::RadioButton(Globals::saveFiles[i].name.c_str(), selectedSave == i)) {
                    selectedSave = i;
                }
            }
            if (Globals::saveFiles.size() > 0) {
                if (UiStyle::Button("Select Save & Start Game!")) {
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

            UiStyle::NextWindow(0.5f);
            ImGui::Begin("Solo VS AI", &Globals::SoloVSAIOpen, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);

            UiStyle::WindowFont();

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

            float buttonWidth = UiStyle::ButtonWidth(2);

            if (UiStyle::Button("Close", buttonWidth)) {
                Globals::SoloVSAIOpen = false;
            }

            if (commandToExecute != nullptr) {
                ImGui::SameLine();

                if (UiStyle::Button("Play!", buttonWidth)) {
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

            UiStyle::NextWindow(0.5f);
            ImGui::Begin("Campaign", &Globals::SoloVSAIOpen, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);

            UiStyle::WindowFont();

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

            float buttonWidth = UiStyle::ButtonWidth(2);

            if (UiStyle::Button("Close", buttonWidth)) {
                Globals::CampaignOpen = false;
            }

            if (commandToExecute != nullptr) {
                ImGui::SameLine();

                if (UiStyle::Button("Play!", buttonWidth)) {
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

            UiStyle::NextWindow(0.5f);
            ImGui::Begin("Operations", &Globals::SoloVSAIOpen, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);

            UiStyle::WindowFont();

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

            float buttonWidth = UiStyle::ButtonWidth(2);

            if (UiStyle::Button("Close", buttonWidth)) {
                Globals::OperationsOpen = false;
            }

            if (commandToExecute != nullptr) {
                ImGui::SameLine();

                if (UiStyle::Button("Play!", buttonWidth)) {
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

            UiStyle::NextWindow();
            ImGui::Begin("New Save", &Globals::NewSaveOpen, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

            UiStyle::WindowFont();

            ImGui::TextWrapped("Display Name (Visible in Multiplayer & Singleplayer)");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##savename", &saveName);
            /*
            if (ImGui::RadioButton("Earn Gear, Mutations & Character Skins by Playing!", !startWithEverything)) {
                startWithEverything = false;
            }
            ImGui::SameLine();
            */
            if (ImGui::RadioButton("Start With Everything!", startWithEverything)) {
                startWithEverything = true;
            }
            if (UiStyle::Button("Create Save File!")) {
                Globals::NewSaveOpen = false;
                Metagame::CreateNewSave(saveName, startWithEverything);
            }
            ImGui::End();
        }

        // The server turned this player away: its message stays up until OK, even after the game
        // has dropped back to the menu. Drawn last so it sits on top of everything else.
        if (Access::HasRefusal()) {
            UiStyle::NextWindow();
            ImGui::SetNextWindowFocus();
            ImGui::Begin("Reborn", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse
                | ImGuiWindowFlags_NoSavedSettings);
            UiStyle::WindowFont();
            ImGui::TextWrapped("%s", Access::Refusal().c_str());
            if (UiStyle::Button("OK"))
                AcknowledgeRefusal();
            ImGui::End();
        }
    }

    void AcknowledgeRefusal() {
        Access::ClearRefusal();
    }
}
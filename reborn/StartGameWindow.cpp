#include "StartGameWindow.hpp"

#include "Globals.hpp"
#include "GameCoordinator.hpp"
#include "LocalHost.hpp"
#include "Overlay.hpp"
#include "StartKey.hpp"
#include "UiStyle.hpp"

#include <algorithm>
#include <atomic>
#include <thread>

namespace StartGameWindow {
    namespace {
        // Bumped when a server start is requested and again when the player cancels. A start that finishes
        // after a cancel sees the number has moved on and gives the server game back instead of joining it.
        std::atomic<int> g_startRequestId{0};
        double g_lastRefresh = 0.0;
        bool g_seenOnce = false;

        void StartOnServer(const StartGameModel& m) {
            StartGame::Request req;
            req.name = StartKey::TrimKey(m.gameName);
            req.mapMode = m.MapMode().first;
            req.mapCommand = m.MapMode().second;
            req.players = m.players;
            req.botsTeamA = m.IsVersus() ? m.botsA : 0;
            req.botsTeamB = m.IsVersus() ? m.botsB : 0;
            req.draftStyle = m.IsVersus() ? m.DraftStyleName() : "QuickSelect";
            req.playerName = StartKey::PlayerName();
            Globals::StartGameStatus = "Starting your game...";
            Globals::PendingStartSince = ImGui::GetTime();
            const int requestId = ++g_startRequestId;
            std::thread([req, requestId]() {
                GameCoordinator::StartResult r = GameCoordinator::StartGame(req);
                if (requestId != g_startRequestId.load()) {
                    // The player cancelled while the server was still starting it: give the game back.
                    if (r.ok) GameCoordinator::CancelGame(r.guid);
                    return;
                }
                if (r.ok) {
                    Globals::CurrentMatchEntry.ServerConnectString = r.connectString;
                    Globals::PendingStartGuid = r.guid;
                }
                else {
                    Globals::StartGameStatus = "PROBLEM: " + r.error;
                }
            }).detach();
        }

        void CancelServerStart() {
            ++g_startRequestId;   // a start still in flight will hand its game back when it finishes
            if (!Globals::PendingStartGuid.empty()) {
                std::string guid = Globals::PendingStartGuid;
                std::thread([guid]() { GameCoordinator::CancelGame(guid); }).detach();
            }
            Globals::PendingStartGuid.clear();
            Globals::StartGameStatus.clear();
            g_seenOnce = false;
        }

        // Follows a game the server is starting; joins it once the server browser lists it as ready.
        void FollowServerStart() {
            if (Globals::PendingStartGuid.empty()) return;
            if (ImGui::GetTime() - g_lastRefresh > 3.0) {
                g_lastRefresh = ImGui::GetTime();
                std::thread(GameCoordinator::RefreshServerBrowser).detach();
            }
            auto e = GameCoordinator::FindEntry(Globals::PendingStartGuid);
            if (e && e->Ready) {
                Globals::CurrentMatchEntry = *e;
                Globals::amStandalone = false;
                Globals::ConnectedToGameCoordinatorMatch = true;
                std::wstring cmd(e->ServerConnectString.begin(), e->ServerConnectString.end());
                Globals::PendingStartGuid.clear();
                Globals::StartGameStatus.clear();
                Globals::StartGameOpen = false;
                g_seenOnce = false;
                Overlay::StartLaunchSequence(_wcsdup(cmd.c_str()));
                return;
            }
            if (e) g_seenOnce = true;
            const bool vanished = g_seenOnce && !e;
            if (vanished || ImGui::GetTime() - Globals::PendingStartSince > 150.0) {
                Globals::StartGameStatus = "PROBLEM: the server did not start. Its log is on the host machine (matchmaking.log).";
                std::string guid = Globals::PendingStartGuid;
                std::thread([guid]() { GameCoordinator::CancelGame(guid); }).detach();   // do not leave it holding a slot
                Globals::PendingStartGuid.clear();
                g_seenOnce = false;
            }
        }

        // A label on its own line, then the control under it fills the row.
        void Label(const char* text) {
            ImGui::TextUnformatted(text);
            ImGui::SetNextItemWidth(-1.0f);
        }

        void DrawForm(StartGameModel& m, bool busy) {
            static std::string filter, lastDefault;

            if (busy) ImGui::BeginDisabled();

            ImGui::TextUnformatted("Run on:");
            ImGui::SameLine();
            if (!m.ServerChoiceEnabled()) ImGui::BeginDisabled();
            if (ImGui::RadioButton("the server", m.runOnServer)) m.runOnServer = true;
            if (!m.ServerChoiceEnabled()) {
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("needs reborn-key.txt (from the host, in the game's Win64 folder)");
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("my PC", !m.runOnServer)) m.runOnServer = false;
            if (!m.ServerChoiceEnabled()) ImGui::TextDisabled("The server needs reborn-key.txt.");

            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("##mapmode", m.MapMode().first.c_str(), ImGuiComboFlags_HeightLarge)) {
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputTextWithHint("##filter", "Filter maps and modes", &filter);
                for (int i = 0; i < (int)Constants::GameCreationOptions.size(); i++) {
                    const auto& option = Constants::GameCreationOptions[i];
                    if (filter.empty() || option.first.contains(filter)) {
                        if (ImGui::Selectable(option.first.c_str(), m.mapIndex == i)) m.mapIndex = i;
                    }
                }
                ImGui::EndCombo();
            }

            m.players = std::clamp(m.players, 1, m.MaxPlayers());
            Label("Players to wait for");
            ImGui::SliderInt("##players", &m.players, 1, m.MaxPlayers());

            if (m.IsVersus()) {
                Label("Bots on team A");
                ImGui::SliderInt("##botsA", &m.botsA, 0, 5);
                Label("Bots on team B");
                ImGui::SliderInt("##botsB", &m.botsB, 0, 5);
                const auto& styles = Constants::DraftStyleNames;
                m.draftStyleIndex = std::clamp(m.draftStyleIndex, 0, (int)styles.size() - 1);
                Label("Draft style");
                if (ImGui::BeginCombo("##draftstyle", styles[m.draftStyleIndex].first.c_str())) {
                    for (int i = 0; i < (int)styles.size(); i++)
                        if (ImGui::Selectable(styles[i].first.c_str(), m.draftStyleIndex == i)) m.draftStyleIndex = i;
                    ImGui::EndCombo();
                }
            }

            if (m.runOnServer) {
                // The name follows the map until the player types their own.
                std::string def = StartKey::DefaultGameName(StartKey::PlayerName(), m.MapMode().first);
                if (m.gameName == lastDefault) m.gameName = def;
                lastDefault = def;
                Label("Game name");
                ImGui::InputText("##gamename", &m.gameName);
                ImGui::TextWrapped("The server starts the game now; you join as soon as it is ready. Friends find it in the server browser.");
            }
            else {
                ImGui::TextWrapped("Your PC hosts the game. Friends join with Direct Connect to your PC's address (UDP port 7777).");
            }

            if (busy) ImGui::EndDisabled();
        }
    }

    void Draw(StartGameModel& m) {
        m.hasKey = !StartKey::Key().empty();
        static bool chosenOnce = false;
        if (!chosenOnce) { m.runOnServer = m.hasKey; chosenOnce = true; }   // the server first, when allowed
        if (!m.ServerChoiceEnabled()) m.runOnServer = false;

        // The server started on this PC is ready: join it like a Direct Connect to this PC.
        if (LocalHost::TakeReadyToJoin()) {
            Globals::amStandalone = false;
            Globals::ConnectedToGameCoordinatorMatch = false;
            Globals::StartGameOpen = false;
            Overlay::StartLaunchSequence(L"open 127.0.0.1");
        }
        FollowServerStart();

        const LocalHost::Status host = LocalHost::GetStatus();
        const bool hostBusy = host == LocalHost::Status::Starting || host == LocalHost::Status::Failed;
        const bool serverBusy = !Globals::StartGameStatus.empty() || !Globals::PendingStartGuid.empty();
        if (!Globals::StartGameOpen && !hostBusy && !serverBusy) return;

        UiStyle::NextWindow();
        ImGui::Begin("Start a Game", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
        UiStyle::WindowFont();

        DrawForm(m, hostBusy || serverBusy);

        // One status area for both ways of starting, with Cancel (or Close after a problem).
        if (hostBusy || serverBusy) {
            ImGui::Separator();
            const bool problem = serverBusy ? Globals::StartGameStatus.rfind("PROBLEM:", 0) == 0
                                            : host == LocalHost::Status::Failed;
            ImGui::TextWrapped("%s", serverBusy ? Globals::StartGameStatus.c_str() : LocalHost::GetStatusText().c_str());
            if (UiStyle::Button(problem ? "Close" : "Cancel")) {
                if (serverBusy) CancelServerStart();
                else if (problem) LocalHost::Dismiss();
                else LocalHost::Stop();
            }
        }
        else {
            const bool canStart = !m.runOnServer || !StartKey::TrimKey(m.gameName).empty();
            const float w = UiStyle::ButtonWidth(2);
            if (!canStart) ImGui::BeginDisabled();
            if (UiStyle::Button("Start", w)) {
                if (m.runOnServer) StartOnServer(m);
                else LocalHost::Start(m.HostMapUrl(), m.MapMode().first, m.players, m.HostDraftStyleValue());
            }
            if (!canStart) ImGui::EndDisabled();
            ImGui::SameLine();
            if (UiStyle::Button("Close", w)) Globals::StartGameOpen = false;
        }

        ImGui::End();
    }
}

#include "PlayerUpdateUi.hpp"

#include "GameCoordinator.hpp"
#include "Globals.hpp"
#include "LaunchOptions.hpp"
#include "PlayerUpdate.hpp"
#include "StartKey.hpp"
#include "UiStyle.hpp"
#include "UpdateKey.hpp"
#include "Version.hpp"
#include "imgui/imgui.h"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

namespace PlayerUpdateUi {
    namespace {
        enum class Stage { None, Offer, Downloading, Ready, Failed };

        std::mutex g_lock;                  // guards the three below (check/download threads vs the overlay)
        Stage g_stage = Stage::None;
        PlayerUpdate::Manifest g_manifest;
        std::string g_failure;

        std::atomic<bool> g_started{ false };
        std::atomic<bool> g_atMenu{ false };

        void SetStage(Stage s, const std::string& failure = "") {
            std::lock_guard<std::mutex> l(g_lock);
            g_stage = s;
            g_failure = failure;
        }

        httplib::Client NewClient(int readTimeoutSeconds) {
            httplib::Client c(GameCoordinator::Endpoint());
            c.set_connection_timeout(3);
            c.set_read_timeout(readTimeoutSeconds);
            return c;
        }

        enum class Got { Ok, Failed, TooBig };

        // GET path into body, refusing an answer over limit bytes or any status but 200.
        Got Fetch(httplib::Client& c, const std::string& path, size_t limit, std::string& body, std::string& why) {
            body.clear();
            bool tooBig = false;
            int status = 0;
            httplib::Result res = c.Get(path,
                [&](const httplib::Response& r) { status = r.status; return r.status == 200; },
                [&](const char* data, size_t n) {
                    if (n > limit - body.size()) { tooBig = true; return false; }
                    body.append(data, n);
                    return true;
                });
            if (tooBig) { why = "the answer was over " + std::to_string(limit) + " bytes"; return Got::TooBig; }
            if (status != 0 && status != 200) { why = "HTTP " + std::to_string(status); return Got::Failed; }
            if (!res) { why = httplib::to_string(res.error()); return Got::Failed; }
            if (res->status != 200) { why = "HTTP " + std::to_string(res->status); return Got::Failed; }
            return Got::Ok;
        }

        // Background thread: one look at the server's player update. Anything wrong is one log line.
        void Check() {
            try {
                httplib::Client c = NewClient(10);
                std::string body, why;
                if (Fetch(c, "/api/updates/player", kMaxManifestResponse, body, why) != Got::Ok) {
                    std::printf("[UPDATE] no player update from %s (%s)\n", GameCoordinator::Endpoint().c_str(), why.c_str());
                    return;
                }
                nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
                if (j.is_discarded() || !j.is_object() || !j.contains("manifest") || !j["manifest"].is_string()
                    || !j.contains("signature") || !j["signature"].is_string()) {
                    std::printf("[UPDATE] the server's update answer could not be read; ignoring it\n");
                    return;
                }
                const std::string manifest = j["manifest"].get<std::string>();
                const std::string signature = j["signature"].get<std::string>();
                if (!PlayerUpdate::VerifySignature(manifest, signature, UpdateKey::PublicKey)) {
                    std::printf("[UPDATE] the server's update is not signed with the Reborn key; ignoring it\n");
                    return;
                }
                std::optional<PlayerUpdate::Manifest> m = PlayerUpdate::ParseManifest(manifest);
                if (!m) {
                    std::printf("[UPDATE] the server's update list could not be read; ignoring it\n");
                    return;
                }
                if (!PlayerUpdate::ShouldOffer(*m, Version::Build)) {
                    std::printf("[UPDATE] the server has Reborn %s; this is %s, so there is nothing newer\n",
                                m->version.c_str(), Version::Text);
                    return;
                }
                std::printf("[UPDATE] offering Reborn %s (this is %s)\n", m->version.c_str(), Version::Text);
                std::lock_guard<std::mutex> l(g_lock);
                g_manifest = *m;
                g_stage = Stage::Offer;
            } catch (...) {
                std::printf("[UPDATE] the update check failed\n");
            }
        }

        // Background thread: download every file, then let PlayerUpdate check and swap them.
        void Download(PlayerUpdate::Manifest m) {
            std::string reason;
            try {
                httplib::Client c = NewClient(60);
                std::map<std::string, std::string> bytes;
                for (const PlayerUpdate::FileEntry& f : m.files) {
                    std::string body, why;
                    Got got = Fetch(c, "/api/updates/player/files/" + f.name, (size_t)f.size, body, why);
                    if (got != Got::Ok) {
                        std::printf("[UPDATE] downloading %s failed (%s)\n", f.name.c_str(), why.c_str());
                        reason = got == Got::TooBig ? "the download was damaged" : "the download did not finish";
                        break;
                    }
                    bytes[f.name] = std::move(body);
                }
                if (reason.empty()) {
                    PlayerUpdate::InstallOutcome out = PlayerUpdate::InstallFiles(StartKey::Win64Dir(), m, bytes);
                    if (out.ok) {
                        std::printf("[UPDATE] installed Reborn %s; the game uses it from the next start\n", m.version.c_str());
                        SetStage(Stage::Ready);
                        return;
                    }
                    std::printf("[UPDATE] installing Reborn %s failed: %s\n", m.version.c_str(), out.message.c_str());
                    reason = InstallFailureReason(out.message);
                }
            } catch (...) {
                std::printf("[UPDATE] the update download failed\n");
                reason = "the download did not finish";
            }
            SetStage(Stage::Failed, FailureText(reason));
        }
    }

    void StartCheckOnce() {
        if (g_started.exchange(true)) return;
        const auto& opt = LaunchOptions::Get();
        std::string why = WhyNoCheck(Globals::amServer, opt.autopilot, opt.noUpdate, Version::Build);
        if (!why.empty()) {
            std::printf("[UPDATE] not checking for a player update (%s)\n", why.c_str());
            return;
        }
        std::printf("[UPDATE] checking %s for a player update\n", GameCoordinator::Endpoint().c_str());
        std::thread(Check).detach();
    }

    void Tick() {
        // The world's name says whether this is the main menu; re-read it when the world changes
        // and twice a second (a new map can reuse the old world's address).
        static UWorld* last = nullptr;
        static ULONGLONG lastRead = 0;
        UWorld* w = Globals::GetGWorld();
        ULONGLONG now = GetTickCount64();
        if (w == last && now - lastRead < 500) return;
        last = w;
        lastRead = now;
        g_atMenu = w && w->GetFullName().find("MenuMap") != std::string::npos;
    }

    void Render() {
        // A key that answered the notice is kept from the game until it is let go, so its release
        // does not also press whatever the game's menu has selected.
        static bool holdKeys = false;
        if (holdKeys) {
            if (ImGui::IsKeyDown(ImGuiKey_Enter) || ImGui::IsKeyDown(ImGuiKey_KeypadEnter) || ImGui::IsKeyDown(ImGuiKey_Escape))
                ImGui::SetNextFrameWantCaptureKeyboard(true);
            else
                holdKeys = false;
        }

        if (!g_atMenu) return;   // never over a match or a loading screen

        Stage stage;
        std::string text, notes;
        {
            std::lock_guard<std::mutex> l(g_lock);
            stage = g_stage;
            if (stage == Stage::Offer) { text = OfferText(g_manifest.version); notes = g_manifest.notes; }
            else if (stage == Stage::Ready) text = kReadyText;
            else if (stage == Stage::Failed) text = g_failure;
        }
        if (stage == Stage::None) return;

        UiStyle::NextWindow();
        ImGui::SetNextWindowFocus();
        // "###" keeps this window apart from the refusal notice, which has the same title.
        ImGui::Begin("Reborn###PlayerUpdate", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
            | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
        UiStyle::WindowFont();

        // Enter presses the first button and Escape the last; while the notice is up the game
        // does not also get those keys.
        const bool focused = ImGui::IsWindowFocused();
        const bool enter = focused && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
        const bool escape = focused && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (stage != Stage::Downloading) ImGui::SetNextFrameWantCaptureKeyboard(true);
        if (stage != Stage::Downloading && (enter || escape)) holdKeys = true;

        if (stage == Stage::Offer) {
            ImGui::TextWrapped("%s", text.c_str());
            if (!notes.empty()) ImGui::TextWrapped("%s", notes.c_str());
            float w = UiStyle::ButtonWidth(2);
            bool download = UiStyle::Button("Download", w) || enter;
            ImGui::SameLine();
            bool notNow = UiStyle::Button("Not now", w) || escape;
            if (download) {
                PlayerUpdate::Manifest m;
                {
                    std::lock_guard<std::mutex> l(g_lock);
                    m = g_manifest;
                    g_stage = Stage::Downloading;
                }
                std::printf("[UPDATE] the player chose Download\n");
                std::thread(Download, std::move(m)).detach();
            }
            else if (notNow) {
                std::printf("[UPDATE] the player chose Not now\n");
                SetStage(Stage::None);
            }
        }
        else if (stage == Stage::Downloading) {
            ImGui::TextWrapped("Downloading the update...");
        }
        else {
            ImGui::TextWrapped("%s", text.c_str());
            if (UiStyle::Button("OK") || enter || escape) SetStage(Stage::None);
        }
        ImGui::End();
    }

    void CleanupAtStart() {
        std::printf("[UPDATE] this is Reborn %s (build %d)\n", Version::Text, Version::Build);
        if (Globals::amServer) return;   // only players install updates
        const std::filesystem::path win64 = StartKey::Win64Dir();
        std::error_code ec;
        if (std::filesystem::exists(win64 / "reborn.dll.bad", ec))
            std::printf("[UPDATE] the last update did not start, so the previous version was put back\n");
        PlayerUpdate::CleanupOld(win64);
    }
}

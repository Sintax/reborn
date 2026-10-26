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

#include <algorithm>
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
        std::atomic<bool> g_rolledBack{ false };   // dxgi put the previous mod back at this start
        std::atomic<bool> g_installing{ false };   // InstallFiles is running (the exit waits for it)

        void SetStage(Stage s, const std::string& failure = "") {
            std::lock_guard<std::mutex> l(g_lock);
            g_stage = s;
            g_failure = failure;
        }

        // Fetch sets the read timeout for each request.
        httplib::Client NewClient() {
            httplib::Client c(GameCoordinator::Endpoint());
            c.set_connection_timeout(3);
            return c;
        }

        enum class Got { Ok, Failed, TooBig, TooSlow };

        // GET path into body, refusing an answer over limit bytes, any status but 200, and anything
        // still arriving once the deadline (started at startMs, kDownloadLimitMs long) has passed.
        Got Fetch(httplib::Client& c, const std::string& path, size_t limit, ULONGLONG startMs, ULONGLONG maxReadSeconds,
                  std::string& body, std::string& why) {
            body.clear();
            ULONGLONG left = RemainingMs(startMs, GetTickCount64());
            if (left == 0) { why = "out of time"; return Got::TooSlow; }
            // A stalled connection must not outlast the deadline either.
            c.set_read_timeout((time_t)std::min<ULONGLONG>(maxReadSeconds, (left + 999) / 1000), 0);
            bool tooBig = false, tooSlow = false;
            int status = 0;
            httplib::Result res = c.Get(path,
                [&](const httplib::Response& r) { status = r.status; return r.status == 200; },
                [&](const char* data, size_t n) {
                    if (RemainingMs(startMs, GetTickCount64()) == 0) { tooSlow = true; return false; }
                    if (n > limit - body.size()) { tooBig = true; return false; }
                    body.append(data, n);
                    return true;
                });
            if (tooSlow || (!res && RemainingMs(startMs, GetTickCount64()) == 0)) { why = "out of time"; return Got::TooSlow; }
            if (tooBig) { why = "the answer was over " + std::to_string(limit) + " bytes"; return Got::TooBig; }
            if (status != 0 && status != 200) { why = "HTTP " + std::to_string(status); return Got::Failed; }
            if (!res) { why = httplib::to_string(res.error()); return Got::Failed; }
            if (res->status != 200) { why = "HTTP " + std::to_string(res->status); return Got::Failed; }
            return Got::Ok;
        }

        // Background thread: one look at the server's player update. Anything wrong is one log line.
        void Check() {
            try {
                const ULONGLONG start = GetTickCount64();
                httplib::Client c = NewClient();
                std::string body, why;
                if (Fetch(c, "/api/updates/player", kMaxManifestResponse, start, 10, body, why) != Got::Ok) {
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

        // Keeps the game's exit waiting while InstallFiles runs (see WaitForInstallBeforeExit).
        struct InstallingFlag {
            InstallingFlag() { g_installing = true; }
            ~InstallingFlag() { g_installing = false; }
        };

        // Background thread: download every file, then let PlayerUpdate check and swap them.
        // startMs is when the player pressed Download; all the files share one 5-minute deadline.
        void Download(PlayerUpdate::Manifest m, ULONGLONG startMs) {
            std::string reason;
            try {
                httplib::Client c = NewClient();
                std::map<std::string, std::string> bytes;
                for (const PlayerUpdate::FileEntry& f : m.files) {
                    std::string body, why;
                    Got got = Fetch(c, "/api/updates/player/files/" + f.name, (size_t)f.size, startMs, 60, body, why);
                    if (got != Got::Ok) {
                        std::printf("[UPDATE] downloading %s failed (%s)\n", f.name.c_str(), why.c_str());
                        reason = got == Got::TooBig ? "the download was damaged" : "the download did not finish";
                        break;
                    }
                    bytes[f.name] = std::move(body);
                }
                if (reason.empty()) {
                    PlayerUpdate::InstallOutcome out;
                    {
                        InstallingFlag installing;
                        out = PlayerUpdate::InstallFiles(StartKey::Win64Dir(), m, bytes);
                    }
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
        std::string why = WhyNoCheck(Globals::amServer, opt.autopilot, opt.noUpdate, Version::Build, g_rolledBack);
        if (!why.empty()) {
            std::printf("[UPDATE] not checking for a player update (%s)\n", why.c_str());
            return;
        }
        std::printf("[UPDATE] checking %s for a player update\n", GameCoordinator::Endpoint().c_str());
        try {
            std::thread(Check).detach();
        } catch (...) {
            // The check is silent by design: no notice, just the log line.
            std::printf("[UPDATE] could not start the update check\n");
        }
    }

    void WaitForInstallBeforeExit() {
        // InstallFiles swaps reborn.dll first and dxgi.dll second, each by two renames
        // (<name> -> <name>.old, then <name>.new -> <name>). If the game quits in between, the
        // install thread dies mid-swap. The worst moment is after dxgi.dll -> dxgi.dll.old and
        // before dxgi.dll.new -> dxgi.dll: the next start finds no dxgi.dll, Windows loads its own,
        // the mod never loads, and nothing can repair it (the rollback lives in our dxgi.dll).
        // So the exit waits for the install to finish (well under a second), at most 10 s.
        if (!g_installing) return;
        std::printf("[UPDATE] waiting for the update to finish installing before the game closes\n");
        std::fflush(stdout);
        const ULONGLONG start = GetTickCount64();
        while (g_installing && GetTickCount64() - start < kExitWaitMs) Sleep(50);
        std::printf("[UPDATE] %s\n", g_installing ? "gave up waiting for the install after 10 s" : "install finished; closing");
        std::fflush(stdout);
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

        // When the current stage was first drawn (for the offer's key delay). Hidden counts as not
        // drawn, so coming back to the menu starts the delay again.
        static Stage shownStage = Stage::None;
        static double shownAt = 0.0;

        if (!g_atMenu) { shownStage = Stage::None; return; }   // never over a match or a loading screen

        Stage stage;
        std::string text, notes;
        {
            std::lock_guard<std::mutex> l(g_lock);
            stage = g_stage;
            if (stage == Stage::Offer) { text = OfferText(g_manifest.version); notes = g_manifest.notes; }
            else if (stage == Stage::Ready) text = kReadyText;
            else if (stage == Stage::Failed) text = g_failure;
        }
        if (stage == Stage::None) { shownStage = Stage::None; return; }

        if (stage != shownStage) { shownStage = stage; shownAt = ImGui::GetTime(); }
        const bool keysAccepted = stage != Stage::Offer || OfferKeysAccepted(ImGui::GetTime() - shownAt);

        UiStyle::NextWindow();
        ImGui::SetNextWindowFocus();
        // "###" keeps this window apart from the refusal notice, which has the same title.
        ImGui::Begin("Reborn###PlayerUpdate", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
            | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
        UiStyle::WindowFont();

        // Enter presses the first button and Escape the last; while the notice is up the game
        // does not also get those keys.
        const bool focused = ImGui::IsWindowFocused() && keysAccepted;
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
                try {
                    std::thread(Download, std::move(m), GetTickCount64()).detach();
                } catch (...) {
                    std::printf("[UPDATE] could not start the download\n");
                    SetStage(Stage::Failed, FailureText("the download could not be started"));
                }
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
        if (std::filesystem::exists(win64 / "reborn.dll.bad", ec)) {
            g_rolledBack = true;   // StartCheckOnce skips this launch's check (WhyNoCheck)
            std::printf("[UPDATE] the last update did not start, so the previous version was put back\n");
        }
        PlayerUpdate::CleanupOld(win64);
    }
}

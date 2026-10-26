#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// Player updates, the in-game part: once per launch, at the main menu, ask the server whether a
// newer signed mod exists, offer it, download it and install it (PlayerUpdate does the checking
// and the file swap). The text helpers below are inline and game-free, so the unit tests use them.
namespace PlayerUpdateUi {

// The largest answer accepted for the manifest request.
constexpr std::size_t kMaxManifestResponse = 64 * 1024;

inline std::string OfferText(std::string_view version) {
    return "Update available: Reborn " + std::string(version);
}

inline constexpr const char* kReadyText = "Update ready. Restart the game to use it.";

inline std::string FailureText(std::string_view reason) {
    return "The update could not be installed: " + std::string(reason) + ". You can keep playing.";
}

// A short reason for FailureText from PlayerUpdate::InstallFiles' message.
inline std::string InstallFailureReason(std::string_view installMessage) {
    if (installMessage.find("damaged") != std::string_view::npos) return "the download was damaged";
    if (installMessage.find("in use") != std::string_view::npos)
        return "a game file is in use. Please try again after restarting the game";
    if (installMessage.find("put back") != std::string_view::npos)
        return "the old files could not all be put back. Please reinstall Battleborn Reborn";
    return "the new files could not be saved";
}

// Why this launch does not check for an update, or "" when it should check.
inline std::string WhyNoCheck(bool isServer, bool autopilot, bool noUpdate, int build) {
    if (isServer) return "this is a game server";
    if (autopilot) return "autopilot test player";
    if (noUpdate) return "-rbnoupdate";
    if (build <= 0) return "developer build";
    return "";
}

// Game: the main menu is up; starts this launch's one background check (later calls do nothing).
void StartCheckOnce();

// Game thread, every tick: notes whether the main menu is up, so the notice is only drawn there.
void Tick();

// Overlay: draws the notice ("Reborn") while there is something to say and the main menu is up.
void Render();

// Startup, before anything else uses the mod files: removes what an earlier update left behind.
void CleanupAtStart();

}  // namespace PlayerUpdateUi

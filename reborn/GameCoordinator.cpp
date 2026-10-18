#include "GameCoordinator.hpp"

#include "Globals.hpp"
#include "Constants.hpp"
#include "LaunchOptions.hpp"
#include "StartKey.hpp"

namespace GameCoordinator {
    std::string Endpoint() {
        const auto& opt = LaunchOptions::Get();
        return opt.coordinator.empty() ? Constants::GameCoordinatorEndpoint : opt.coordinator;
    }

    httplib::Client& Client() {
        if (!Globals::GameCoordinatorHttpClient.get()) {
            Globals::GameCoordinatorHttpClient = std::make_shared<httplib::Client>(Endpoint());
            // A service that is down should cost a few seconds, not a frozen menu.
            Globals::GameCoordinatorHttpClient->set_connection_timeout(3);
            Globals::GameCoordinatorHttpClient->set_read_timeout(5);
        }
        return *Globals::GameCoordinatorHttpClient;
    }
}

namespace GameCoordinator {
    // Guards Globals::ServerBrowserEntries between RefreshServerBrowser (a worker thread) and FindEntry (the UI thread).
    static std::mutex g_browserEntriesMutex;

    void RefreshServerBrowser() {
        httplib::Result result;

        Client();

        result = Client().Get("/api/games");

        if (!result || result->status != 200) {
            std::cout << "[NETWORKING] Failed to refresh server list from " << Endpoint() << ": "
                      << (result ? "HTTP " + std::to_string(result->status) : httplib::to_string(result.error())) << std::endl;
            return;
        }

        std::vector<nlohmann::json> entries = nlohmann::json::parse(result->body).get<std::vector<nlohmann::json>>();

        // Build the new list on the side, then swap it in, so a reader never sees a half-filled list.
        std::vector<ServerBrowserEntry> fresh;
        for (nlohmann::json browserEntry : entries) {
            fresh.push_back(ServerBrowserEntry(browserEntry));
        }

        std::lock_guard<std::mutex> lock(g_browserEntriesMutex);
        Globals::ServerBrowserEntries = std::move(fresh);

        return;
    }
}

namespace GameCoordinator {
    // Start and cancel run on their own threads, and the server may take a while to
    // spawn the game, so they get a private client instead of the shared Client().
    static httplib::Client StartClient() {
        httplib::Client c(Endpoint());
        c.set_connection_timeout(3);
        c.set_read_timeout(75);
        return c;
    }

    static std::string ErrorMessage(const httplib::Result& res, const char* fallback) {
        if (!res) return "Could not reach the server (" + httplib::to_string(res.error()) + ").";
        try {
            auto j = nlohmann::json::parse(res->body);
            if (j.contains("message") && j["message"].is_string()) {
                std::string m = j["message"].get<std::string>();
                if (!m.empty()) return m;
            }
        } catch (...) {}
        return std::string(fallback) + " (HTTP " + std::to_string(res->status) + ").";
    }

    StartResult StartGame(const StartGame::Request& request) {
        StartResult out;
        std::string key = StartKey::Key();
        if (key.empty()) { out.error = "You need a server key from the host (reborn-key.txt in the game's Win64 folder)."; return out; }
        httplib::Headers headers = { {"X-Start-Key", key} };
        httplib::Result res = StartClient().Post("/api/games/start", headers, StartGame::ToJson(request), "application/json");
        if (!res || res->status != 200) { out.error = ErrorMessage(res, "The server refused to start the game"); return out; }
        try {
            auto j = nlohmann::json::parse(res->body);
            out.guid = j.value("guid", std::string());
            out.connectString = j.value("connectString", std::string());
        } catch (...) {
            out.error = "The server's answer could not be read.";
            return out;
        }
        out.ok = true;
        std::printf("[NETWORKING] started \"%s\" on the server: %s (%s), draft style %s\n", request.name.c_str(), out.connectString.c_str(),
                    out.guid.c_str(), request.draftStyle.c_str());
        return out;
    }

    bool CancelGame(const std::string& guid) {
        std::string key = StartKey::Key();
        if (key.empty() || guid.empty()) return false;
        httplib::Headers headers = { {"X-Start-Key", key} };
        httplib::Result res = StartClient().Delete(("/api/games/" + guid).c_str(), headers);
        return res && res->status == 200;
    }

    std::optional<ServerBrowserEntry> FindEntry(const std::string& guid) {
        if (guid.empty()) return std::nullopt;
        std::lock_guard<std::mutex> lock(g_browserEntriesMutex);
        for (const auto& e : Globals::ServerBrowserEntries)
            if (e.MyGuid == guid) return e;
        return std::nullopt;
    }

    std::vector<ServerBrowserEntry> BrowserEntriesSnapshot() {
        std::lock_guard<std::mutex> lock(g_browserEntriesMutex);
        return Globals::ServerBrowserEntries;
    }
}

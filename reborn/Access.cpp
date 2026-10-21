#include "Access.hpp"
#include "AccessRules.hpp"
#include "Globals.hpp"
#include "LaunchOptions.hpp"
#include "Networking.hpp"
#include "PlayerNames.hpp"
#include "StartKey.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace Access {
    namespace {
        namespace fs = std::filesystem;

        const char* kUsersFile = "reborn-users.json";
        const float kCheckSeconds = 2.0f;   // how often the list's last-write time is looked at
        const float kCloseDelay = 1.0f;     // refusal message first, then the close

        // Server state (game thread only).
        bool g_started = false;
        fs::path g_path;
        AccessRules::ReloadState g_state;   // the last good list, and what has been logged
        fs::file_time_type g_lastWrite{};
        float g_sinceCheck = 0.f;

        struct PendingClose { UNetConnection* connection; float secondsLeft; };
        std::vector<PendingClose> g_closing;

        // Client state (written on the game thread, read by the menu code).
        std::mutex g_clientLock;
        bool g_hasRefusal = false;
        std::string g_refusal;

        std::string PathText() { return PlayerNames::ToAscii(g_path.wstring()); }

        std::optional<std::string> ReadWhole(const fs::path& p) {
            std::ifstream f(p, std::ios::binary);
            if (!f) return std::nullopt;
            std::stringstream ss;
            ss << f.rdbuf();
            if (f.bad()) return std::nullopt;
            return ss.str();
        }

        void Report(AccessRules::ReloadLog log) {
            using AccessRules::ReloadLog;
            const std::optional<AccessRules::AccessList>& list = g_state.list;
            switch (log) {
            case ReloadLog::None: break;
            case ReloadLog::NoFileAtStart:
                printf("[ACCESS] no user list: anyone can join\n");
                break;
            case ReloadLog::Loaded:
            case ReloadLog::Reloaded:
                printf("[ACCESS] user list %s%s: %zu users, keys required %s\n", PathText().c_str(),
                    log == ReloadLog::Reloaded ? " (re-read)" : "", list->users.size(), list->requireKeys ? "yes" : "no");
                break;
            case ReloadLog::BadKeptList:
                printf("[ACCESS] PROBLEM: the user list %s could not be read or is not the expected shape; keeping the last good list (%zu users)\n",
                    PathText().c_str(), list->users.size());
                break;
            case ReloadLog::BadRefuseAll:
                printf("[ACCESS] PROBLEM: the user list %s could not be read or is not the expected shape; every join is refused until it is fixed\n",
                    PathText().c_str());
                break;
            case ReloadLog::GoneKeptList:
                // Set RequireKeys false (not delete the file) to open the server.
                printf("[ACCESS] PROBLEM: the user list %s is gone; keeping the last list read\n", PathText().c_str());
                break;
            case ReloadLog::GoneNoGate:
                printf("[ACCESS] the unreadable user list %s is gone and no list was ever read: anyone can join\n", PathText().c_str());
                break;
            }
        }

        // Reads the list if it is new or changed; what that means is AccessRules::ApplyRead. True
        // when a good list was (re)loaded, which is the only time joined players are checked
        // again: a bad or missing file never removes anyone (Review Focus 2).
        bool Load() {
            std::error_code ec;
            bool exists = fs::exists(g_path, ec) && !ec;
            fs::file_time_type written{};
            std::optional<AccessRules::AccessList> parsed;
            if (exists) {
                written = fs::last_write_time(g_path, ec);
                if (ec) written = fs::file_time_type{};
                // Unchanged since the last good read: nothing to do.
                if (g_state.started && g_state.fileSeen && !ec && written == g_lastWrite) return false;
                std::optional<std::string> text = ReadWhole(g_path);
                if (text) parsed = AccessRules::ParseAccessList(*text);
            }
            bool good = parsed.has_value();
            AccessRules::ReloadOutcome outcome = AccessRules::ApplyRead(g_state, exists, std::move(parsed));
            // Only a good read moves g_lastWrite, so a bad file is read again at the next check (a
            // read can also fail while another program has the file open).
            if (good) g_lastWrite = written;
            Report(outcome.log);
            return outcome.listChanged;
        }

        void Start() {
            if (g_started) return;
            g_started = true;
            const std::wstring& given = LaunchOptions::Get().users;
            g_path = given.empty() ? StartKey::Win64Dir() / kUsersFile : fs::path(given);
            Load();
        }

        AccessRules::AccessDecision Decide(const Globals::ServerPlayer& sp) {
            return AccessRules::CheckJoin(g_state.list ? &*g_state.list : nullptr, g_state.unreadable, sp.WantedName, sp.Key);
        }

        // The user whose key the player sent, for the log (name only).
        std::string UserNameFor(const Globals::ServerPlayer& sp) {
            if (!g_state.list) return "";
            for (const AccessRules::AccessUser& u : g_state.list->users)
                if (!u.key.empty() && u.key == sp.Key) return PlayerNames::SafeLogText(u.name);
            return "";
        }

        void Refuse(Globals::ServerPlayer& sp, const char* reason, const char* when) {
            sp.Refused = true;
            sp.Joined = false;   // never logged in (or, if playing, not counted any more)
            std::string message = std::string("RebornRefused=") + AccessRules::kRefusedMessage;
            bool told = PlayerNames::SendControlText(sp.Connection, message);
            printf("[ACCESS] refused \"%s\": %s%s%s\n", PlayerNames::CleanName(sp.WantedName).c_str(), reason, when,
                told ? "" : " (no control channel to tell the player)");
            g_closing.push_back({ sp.Connection, kCloseDelay });
        }

        void CloseDue(float dt) {
            for (size_t i = 0; i < g_closing.size();) {
                PendingClose& pc = g_closing[i];
                pc.secondsLeft -= dt;
                if (pc.secondsLeft > 0.f) { i++; continue; }
                // Only a connection still in the net driver's list is touched (it may have left).
                if (ServerNetworking::IsLiveConnection(pc.connection)) {
                    // UNetConnection::Close (battleborn+0x61a850): closes the control channel, marks
                    // the connection closed and flushes; the engine then cleans it up as for any
                    // player who left (the same call the engine makes on a failed join).
                    reinterpret_cast<void (*)(UNetConnection*)>(Globals::baseAddress + 0x61a850)(pc.connection);
                    printf("[ACCESS] closed the refused player's connection\n");
                }
                g_closing.erase(g_closing.begin() + i);
            }
        }
    }

    void Tick(float dt) {
        Start();
        CloseDue(dt);
        g_sinceCheck += dt;
        if (g_sinceCheck < kCheckSeconds) return;
        g_sinceCheck = 0.f;
        if (!Load()) return;
        // The list changed: anyone playing who no longer passes is removed the same way.
        for (Globals::ServerPlayer& sp : Globals::ServerPlayers) {
            if (!sp.Joined || sp.Refused || !sp.Connection || !ServerNetworking::IsLiveConnection(sp.Connection))
                continue;
            AccessRules::AccessDecision d = Decide(sp);
            if (!d.allowed)
                Refuse(sp, d.reason, " (the user list changed while playing)");
        }
    }

    bool CanJoinNow(const Globals::ServerPlayer& sp) {
        Start();
        return Decide(sp).allowed;
    }

    bool AllowJoin(Globals::ServerPlayer& sp) {
        Start();
        if (sp.Refused) return false;
        AccessRules::AccessDecision d = Decide(sp);
        std::string name = PlayerNames::CleanName(sp.WantedName);
        if (!d.allowed) {
            Refuse(sp, d.reason, "");
            return false;
        }
        if (!g_state.list)
            printf("[ACCESS] \"%s\" may join (no user list)\n", name.c_str());
        else if (!g_state.list->requireKeys)
            printf("[ACCESS] \"%s\" may join (keys not required)\n", name.c_str());
        else
            printf("[ACCESS] \"%s\" may join (user \"%s\")\n", name.c_str(), UserNameFor(sp).c_str());
        return true;
    }

    void OnClientText(void* inbunch) {
        std::string s = PlayerNames::ReadControlText(inbunch);
        const std::string tag = "RebornRefused=";
        if (s.rfind(tag, 0) != 0) return;   // other server text: nothing to do (the engine only logs it)
        std::string text = PlayerNames::SafeLogText(s.substr(tag.size()), 80);
        {
            std::scoped_lock l(g_clientLock);
            g_hasRefusal = true;
            g_refusal = text;
        }
        printf("[ACCESS] client: refused by the server: %s\n", text.c_str());
    }

    bool HasRefusal() {
        std::scoped_lock l(g_clientLock);
        return g_hasRefusal;
    }

    std::string Refusal() {
        std::scoped_lock l(g_clientLock);
        return g_refusal;
    }

    void ClearRefusal() {
        std::scoped_lock l(g_clientLock);
        g_hasRefusal = false;
        g_refusal.clear();
    }
}

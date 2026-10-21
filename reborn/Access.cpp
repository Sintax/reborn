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
        std::optional<AccessRules::AccessList> g_list;   // the last good list
        bool g_unreadable = false;      // the file was bad before any good read: refuse everyone
        bool g_fileSeen = false;        // a file was there at some read
        bool g_problemLogged = false;   // the current bad file has been logged once
        bool g_goneLogged = false;      // the file's removal has been logged once
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

        void LogList(const char* how) {
            printf("[ACCESS] user list %s%s: %zu users, keys required %s\n", PathText().c_str(), how,
                g_list->users.size(), g_list->requireKeys ? "yes" : "no");
        }

        // Reads the list if it is new or changed. True when a good list was (re)loaded, which is
        // the only time joined players are checked again: a bad or missing file never removes
        // anyone (Review Focus 2).
        bool Load(bool first) {
            std::error_code ec;
            bool exists = fs::exists(g_path, ec);
            if (ec || !exists) {
                if (first)
                    printf("[ACCESS] no user list: anyone can join\n");
                else if (g_fileSeen && !g_goneLogged) {
                    // Writers replace it with a rename, so it is never missing mid-write; someone
                    // removed it. Keep what we have (set RequireKeys false to open the server).
                    g_goneLogged = true;
                    if (g_list) {
                        printf("[ACCESS] PROBLEM: the user list %s is gone; keeping the last list read\n", PathText().c_str());
                    } else {
                        // The only file ever seen was unreadable and no good list was read: with the
                        // file gone this is "no file" again, not a reason to refuse everyone.
                        g_unreadable = false;
                        g_problemLogged = false;
                        printf("[ACCESS] the unreadable user list %s is gone and no list was ever read: anyone can join\n", PathText().c_str());
                    }
                }
                return false;
            }
            g_goneLogged = false;
            fs::file_time_type written = fs::last_write_time(g_path, ec);
            if (ec) written = fs::file_time_type{};
            if (!first && g_fileSeen && !ec && written == g_lastWrite) return false;
            g_fileSeen = true;

            std::optional<std::string> text = ReadWhole(g_path);
            std::optional<AccessRules::AccessList> parsed;
            if (text) parsed = AccessRules::ParseAccessList(*text);
            if (!parsed) {
                // g_lastWrite is left alone, so the next check reads it again (a read can also fail
                // while another program has the file open).
                if (!g_problemLogged) {
                    g_problemLogged = true;
                    if (g_list)
                        printf("[ACCESS] PROBLEM: the user list %s could not be read or is not the expected shape; keeping the last good list (%zu users)\n",
                            PathText().c_str(), g_list->users.size());
                    else
                        printf("[ACCESS] PROBLEM: the user list %s could not be read or is not the expected shape; every join is refused until it is fixed\n",
                            PathText().c_str());
                }
                if (!g_list) g_unreadable = true;
                return false;
            }
            bool reload = g_list.has_value() || g_unreadable;
            g_list = std::move(parsed);
            g_unreadable = false;
            g_problemLogged = false;
            g_lastWrite = written;
            LogList(reload ? " (re-read)" : "");
            return true;
        }

        void Start() {
            if (g_started) return;
            g_started = true;
            const std::wstring& given = LaunchOptions::Get().users;
            g_path = given.empty() ? StartKey::Win64Dir() / kUsersFile : fs::path(given);
            Load(true);
        }

        AccessRules::AccessDecision Decide(const Globals::ServerPlayer& sp) {
            return AccessRules::CheckJoin(g_list ? &*g_list : nullptr, g_unreadable, sp.WantedName, sp.Key);
        }

        // The user whose key the player sent, for the log (name only).
        std::string UserNameFor(const Globals::ServerPlayer& sp) {
            if (!g_list) return "";
            for (const AccessRules::AccessUser& u : g_list->users)
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
        if (!Load(false)) return;
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
        if (!g_list)
            printf("[ACCESS] \"%s\" may join (no user list)\n", name.c_str());
        else if (!g_list->requireKeys)
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

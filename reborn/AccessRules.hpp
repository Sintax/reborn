#pragma once

#include "PlayerNameRules.hpp"
#include "json.hpp"

#include <optional>
#include <string>
#include <vector>

// Who may join a Reborn server: the user list file (reborn-users.json) and the join rule, kept
// free of the SDK so the unit tests can build it. Keys must never be printed or logged here or by
// callers; names may.
//
// Join rule (identical meaning in every reader of the file):
//   1. No file: everyone may join.            (list == nullptr, listUnreadable false)
//   2. File present but unreadable/wrong shape: refuse everyone.   (listUnreadable true)
//   3. RequireKeys false: everyone may join.
//   4. Find the user whose Key equals the sent key; none (or no key sent): refuse.
//   5. That user has ConfirmName false and the cleaned names differ (ignoring case): refuse.
//   6. Otherwise allow.
namespace AccessRules {
    // What the player always sees on a refusal. The reason below is for the server log only.
    constexpr const char* kRefusedMessage = "Improper Credentials!";

    struct AccessUser {
        std::string name;
        std::string key;
        bool canCreateGames = false;
        bool confirmName = false;       // true: key alone identifies them (name is only a label)
    };

    struct AccessList {
        bool requireKeys = true;
        std::vector<AccessUser> users;
    };

    struct AccessDecision {
        bool allowed;
        const char* reason;     // "" when allowed; otherwise a fixed string, never containing a key
    };

    // The list from the file's text. nullopt for bad JSON, a missing/ill-typed "Users", or a user
    // entry of the wrong shape (the caller treats that as "unreadable"). The same strict shape as
    // matchmaking (UserList.cs) and tools/vm.py: Name and Key must be non-empty strings;
    // RequireKeys, CanCreateGames and ConfirmName may be left out but are booleans when present.
    // A missing "RequireKeys" means true; missing CanCreateGames/ConfirmName mean false. A leading
    // UTF-8 BOM is tolerated.
    inline std::optional<AccessList> ParseAccessList(const std::string& jsonText) {
        using nlohmann::json;
        json j = json::parse(jsonText, nullptr, /*allow_exceptions=*/false);
        if (j.is_discarded() || !j.is_object()) return std::nullopt;

        AccessList list;
        auto req = j.find("RequireKeys");
        if (req != j.end()) {
            if (!req->is_boolean()) return std::nullopt;
            list.requireKeys = req->get<bool>();
        }

        auto users = j.find("Users");
        if (users == j.end() || !users->is_array()) return std::nullopt;

        for (const json& u : *users) {
            if (!u.is_object()) return std::nullopt;
            AccessUser user;
            auto name = u.find("Name");
            auto key = u.find("Key");
            if (name == u.end() || !name->is_string() || key == u.end() || !key->is_string()) return std::nullopt;
            user.name = name->get<std::string>();
            user.key = key->get<std::string>();
            if (user.name.empty() || user.key.empty()) return std::nullopt;
            auto can = u.find("CanCreateGames");
            if (can != u.end()) { if (!can->is_boolean()) return std::nullopt; user.canCreateGames = can->get<bool>(); }
            auto confirm = u.find("ConfirmName");
            if (confirm != u.end()) { if (!confirm->is_boolean()) return std::nullopt; user.confirmName = confirm->get<bool>(); }
            list.users.push_back(std::move(user));
        }
        return list;
    }

    // `list` is the current good list (nullptr when there is none). `key` is already trimmed
    // (StartKey::TrimKey); it is compared exactly, case included. `name` is cleaned here.
    inline AccessDecision CheckJoin(const AccessList* list, bool listUnreadable,
                                    const std::string& name, const std::string& key) {
        if (listUnreadable) return { false, "user list unreadable" };
        if (list == nullptr) return { true, "" };
        if (!list->requireKeys) return { true, "" };
        if (key.empty()) return { false, "no key" };

        for (const AccessUser& u : list->users) {
            if (u.key.empty() || u.key != key) continue;
            if (!u.confirmName &&
                !PlayerNames::SameName(PlayerNames::CleanName(u.name), PlayerNames::CleanName(name)))
                return { false, "name does not match key" };
            return { true, "" };
        }
        return { false, "unknown key" };
    }

    // What the server knows from reading the user list over time (Access.cpp keeps one).
    struct ReloadState {
        std::optional<AccessList> list;  // the last good list
        bool unreadable = false;         // the file was bad before any good read: refuse everyone
        bool started = false;            // the first read has happened
        bool fileSeen = false;           // a file was there at some read
        bool problemLogged = false;      // the current bad file has been logged once
        bool goneLogged = false;         // the file's removal has been logged once
    };

    // The one log line a read calls for (None: say nothing).
    enum class ReloadLog {
        None,
        NoFileAtStart,      // no file at the first read: anyone can join
        Loaded,             // first good list
        Reloaded,           // a good list replaced an earlier list (or an unreadable file)
        BadRefuseAll,       // PROBLEM: bad file and no good list: every join is refused
        BadKeptList,        // PROBLEM: bad file; the last good list stays
        GoneKeptList,       // PROBLEM: the file is gone; the last good list stays
        GoneNoGate,         // the only file seen was bad and is now gone: anyone can join
    };

    struct ReloadOutcome {
        bool listChanged;   // a good list was (re)loaded: joined players are checked again
        ReloadLog log;
    };

    // Applies one look at the file. `fileExists` false means it is not there; otherwise `parsed`
    // is ParseAccessList's result (nullopt for a file that could not be read or is the wrong
    // shape). A bad or missing file never changes a good list, so nobody playing is removed by it
    // (Review Focus 2). A bad file before any good read refuses everyone until it is fixed; if
    // that file is then removed, it is "no file" again (Ruling 6). Each bad spell or removal is
    // reported once.
    inline ReloadOutcome ApplyRead(ReloadState& s, bool fileExists, std::optional<AccessList> parsed) {
        bool first = !s.started;
        s.started = true;
        if (!fileExists) {
            if (first) return { false, ReloadLog::NoFileAtStart };
            if (!s.fileSeen || s.goneLogged) return { false, ReloadLog::None };
            // Writers replace it with a rename, so it is never missing mid-write: someone removed it.
            s.goneLogged = true;
            if (s.list) return { false, ReloadLog::GoneKeptList };
            s.unreadable = false;
            s.problemLogged = false;
            return { false, ReloadLog::GoneNoGate };
        }
        s.goneLogged = false;
        s.fileSeen = true;
        if (!parsed) {
            ReloadLog log = ReloadLog::None;
            if (!s.problemLogged) {
                s.problemLogged = true;
                log = s.list ? ReloadLog::BadKeptList : ReloadLog::BadRefuseAll;
            }
            if (!s.list) s.unreadable = true;
            return { false, log };
        }
        bool reload = s.list.has_value() || s.unreadable;
        s.list = std::move(parsed);
        s.unreadable = false;
        s.problemLogged = false;
        return { true, reload ? ReloadLog::Reloaded : ReloadLog::Loaded };
    }
}

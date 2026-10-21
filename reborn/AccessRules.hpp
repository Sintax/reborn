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
    // entry of the wrong shape (the caller treats that as "unreadable"). A missing "RequireKeys"
    // means true; missing CanCreateGames/ConfirmName mean false. A leading UTF-8 BOM is tolerated.
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
            if (name != u.end()) { if (!name->is_string()) return std::nullopt; user.name = name->get<std::string>(); }
            if (key != u.end()) { if (!key->is_string()) return std::nullopt; user.key = key->get<std::string>(); }
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
}

// reborn/tests/test_access_rules.cpp
#include "../AccessRules.hpp"
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

// Made-up keys, 24 characters of the real alphabet. They are never printed.
static const char* kSintaxKey = "ABCDEFGHJKMNPQRSTVWXYZ23";
static const char* kKazKey    = "23456789ABCDEFGHJKMNPQRS";

static AccessRules::AccessList MakeList() {
    const std::string json = std::string(R"({
      "RequireKeys": true,
      "Users": [
        { "Name": "dj",  "Key": ")") + kSintaxKey + R"(", "CanCreateGames": true,  "ConfirmName": true,  "Added": "2026-10-21T18:00:00Z" },
        { "Name": "Kaz", "Key": ")" + kKazKey + R"(",    "CanCreateGames": false, "ConfirmName": false, "Added": "2026-10-21T18:05:00Z" }
      ]
    })";
    auto list = AccessRules::ParseAccessList(json);
    if (!list) { std::printf("FAIL: MakeList could not parse its own JSON\n"); failures++; return {}; }
    return *list;
}

// True when CheckJoin on `l` gives exactly `reason` ("" = allowed).
static bool Decides(const AccessRules::AccessList& l, const char* name, const std::string& key, const char* reason = "") {
    AccessRules::AccessDecision d = AccessRules::CheckJoin(&l, false, name, key);
    return d.allowed == (reason[0] == 0) && std::strcmp(d.reason, reason) == 0;
}

static void no_file_allows_everyone() {
    AccessRules::AccessDecision d = AccessRules::CheckJoin(nullptr, false, "Anyone", "");
    CHECK(d.allowed && std::strcmp(d.reason, "") == 0);
    d = AccessRules::CheckJoin(nullptr, false, "Anyone", "whatever");
    CHECK(d.allowed && std::strcmp(d.reason, "") == 0);
}

static void unreadable_list_refuses_everyone() {
    AccessRules::AccessDecision d = AccessRules::CheckJoin(nullptr, true, "Sintax", kSintaxKey);
    CHECK(!d.allowed && std::strcmp(d.reason, "user list unreadable") == 0);
    d = AccessRules::CheckJoin(nullptr, true, "Sintax", "");
    CHECK(!d.allowed && std::strcmp(d.reason, "user list unreadable") == 0);
    // Unreadable wins even if a list pointer is also passed.
    AccessRules::AccessList l = MakeList();
    d = AccessRules::CheckJoin(&l, true, "Sintax", kSintaxKey);
    CHECK(!d.allowed && std::strcmp(d.reason, "user list unreadable") == 0);
}

static void require_keys_off_allows_everyone() {
    AccessRules::AccessList l = MakeList();
    l.requireKeys = false;
    CHECK(Decides(l, "Stranger", ""));
    CHECK(Decides(l, "Stranger", "not-a-key"));
    CHECK(Decides(l, "Mike", kKazKey));          // not even the name check applies
}

static void missing_key_is_refused_as_no_key() {
    AccessRules::AccessList l = MakeList();
    CHECK(Decides(l, "Sintax", "", "no key"));
    // A user stored with an empty key must not let an empty sent key through.
    AccessRules::AccessUser blank{ "Blank", "", false, true };
    l.users.push_back(blank);
    CHECK(Decides(l, "Blank", "", "no key"));
}

static void unknown_key_is_refused() {
    AccessRules::AccessList l = MakeList();
    CHECK(Decides(l, "Sintax", "ZZZZZZZZZZZZZZZZZZZZZZZZ", "unknown key"));
    CHECK(Decides(l, "Sintax", "short", "unknown key"));
}

static void name_must_match_unless_confirm_name() {
    AccessRules::AccessList l = MakeList();
    // Migrated owner key (label "dj", ConfirmName true): plays as Sintax, the key alone identifies them.
    CHECK(Decides(l, "Sintax", kSintaxKey));
    CHECK(Decides(l, "anything-else", kSintaxKey));
    // ConfirmName false: names compare after cleaning, ignoring case.
    CHECK(Decides(l, "Kaz", kKazKey));
    CHECK(Decides(l, "kaz", kKazKey));
    CHECK(Decides(l, "KAZ", kKazKey));
    CHECK(Decides(l, " k a z? ", kKazKey));        // cleaned to "kaz"
    CHECK(Decides(l, "Mike", kKazKey, "name does not match key"));
    CHECK(Decides(l, "", kKazKey, "name does not match key"));   // cleans to "Player"
    CHECK(Decides(l, "Kaz2", kKazKey, "name does not match key"));
    // The stored name is cleaned too.
    l.users.push_back(AccessRules::AccessUser{ "Odd Name!", "KEY0", false, false });
    CHECK(Decides(l, "OddName", "KEY0"));
    CHECK(Decides(l, "oddname", "KEY0"));
}

static void key_compare_is_exact() {
    AccessRules::AccessList l = MakeList();
    std::string lower = kSintaxKey;
    for (char& c : lower) c = (char)std::tolower((unsigned char)c);
    CHECK(Decides(l, "Sintax", lower, "unknown key"));
    CHECK(Decides(l, "Sintax", std::string(kSintaxKey) + "X", "unknown key"));
    CHECK(Decides(l, "Sintax", std::string(kSintaxKey).substr(1), "unknown key"));
    CHECK(Decides(l, "Sintax", std::string(kSintaxKey) + " ", "unknown key"));   // trimming is the caller's job
}

static void parse_rejects_bad_json_and_wrong_shapes() {
    using AccessRules::ParseAccessList;
    CHECK(!ParseAccessList(""));
    CHECK(!ParseAccessList("not json"));
    CHECK(!ParseAccessList("{ \"Users\": [ "));                    // truncated (mid-rewrite)
    CHECK(!ParseAccessList("[]"));
    CHECK(!ParseAccessList("42"));
    CHECK(!ParseAccessList("{}"));                                 // no Users
    CHECK(!ParseAccessList(R"({"RequireKeys": true})"));
    CHECK(!ParseAccessList(R"({"Users": {}})"));                   // Users not an array
    CHECK(!ParseAccessList(R"({"Users": "x"})"));
    CHECK(!ParseAccessList(R"({"Users": null})"));
    CHECK(!ParseAccessList(R"({"RequireKeys": "yes", "Users": []})"));   // RequireKeys not a bool
    CHECK(!ParseAccessList(R"({"Users": [ 5 ]})"));                // a user that is not an object
    CHECK(!ParseAccessList(R"({"Users": [ {"Name": 1, "Key": "K"} ]})"));
    CHECK(!ParseAccessList(R"({"Users": [ {"Name": "A", "Key": 7} ]})"));
    CHECK(!ParseAccessList(R"({"Users": [ {"Name": "A", "Key": "K", "CanCreateGames": "yes"} ]})"));
    CHECK(!ParseAccessList(R"({"Users": [ {"Name": "A", "Key": "K", "ConfirmName": 1} ]})"));

    // Good shapes.
    auto empty = ParseAccessList(R"({"Users": []})");
    CHECK(empty && empty->users.empty());
    auto l = ParseAccessList(R"({"RequireKeys": true, "Users": [ {"Name": "Ann", "Key": "K1", "CanCreateGames": true, "ConfirmName": false, "Added": "x", "Extra": 1} ]})");
    CHECK(l && l->users.size() == 1);
    if (l && l->users.size() == 1) {
        CHECK(l->users[0].name == "Ann" && l->users[0].key == "K1");
        CHECK(l->users[0].canCreateGames && !l->users[0].confirmName);
    }
    // Missing optional per-user flags are false.
    auto m = ParseAccessList(R"({"Users": [ {"Name": "Bo", "Key": "K2"} ]})");
    CHECK(m && m->users.size() == 1 && !m->users[0].canCreateGames && !m->users[0].confirmName);
    // A UTF-8 BOM in front (Notepad) is tolerated.
    CHECK(ParseAccessList("\xEF\xBB\xBF" R"({"Users": []})").has_value());
}

static void parse_defaults_require_keys_to_true() {
    auto l = AccessRules::ParseAccessList(R"({"Users": []})");
    CHECK(l && l->requireKeys);
    auto on = AccessRules::ParseAccessList(R"({"RequireKeys": true, "Users": []})");
    CHECK(on && on->requireKeys);
    auto off = AccessRules::ParseAccessList(R"({"RequireKeys": false, "Users": []})");
    CHECK(off && !off->requireKeys);
    // With the default, an unknown key is refused.
    CHECK(l && !AccessRules::CheckJoin(&*l, false, "A", "K").allowed);
}

static void refused_message_is_exact() {
    CHECK(std::strcmp(AccessRules::kRefusedMessage, "Improper Credentials!") == 0);
}

int main() {
    no_file_allows_everyone();
    unreadable_list_refuses_everyone();
    require_keys_off_allows_everyone();
    missing_key_is_refused_as_no_key();
    unknown_key_is_refused();
    name_must_match_unless_confirm_name();
    key_compare_is_exact();
    parse_rejects_bad_json_and_wrong_shapes();
    parse_defaults_require_keys_to_true();
    refused_message_is_exact();

    if (failures == 0) std::printf("PASS\n");
    return failures == 0 ? 0 : 1;
}

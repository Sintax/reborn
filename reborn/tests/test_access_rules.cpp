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

// The shared list of damaged files: matchmaking (gamecontroller.Tests/UserListTests.cs BadShapes)
// and tools/vm.py (debugloop/tests/test_vm.py BAD_USER_FILES) refuse exactly these too.
static const char* const kBadUserFiles[] = {
    "",
    "not json",
    R"({ "Users": [ )",                                               // truncated (mid-rewrite)
    "[]",
    "42",
    "{}",                                                             // no Users
    R"({"RequireKeys": true})",
    R"({"Users": {}})",                                               // Users not an array
    R"({"Users": "x"})",
    R"({"Users": null})",
    R"({"RequireKeys": "yes", "Users": []})",                         // RequireKeys not a bool
    R"({"RequireKeys": "true", "Users": []})",
    R"({"RequireKeys": 1, "Users": []})",
    R"({"RequireKeys": null, "Users": []})",
    R"({"Users": [5]})",                                              // a user that is not an object
    R"({"Users": [{"Key": "K"}]})",                                   // no Name
    R"({"Users": [{"Name": "A"}]})",                                  // no Key
    R"({"Users": [{"Name": "", "Key": "K"}]})",
    R"({"Users": [{"Name": "A", "Key": ""}]})",
    R"({"Users": [{"Name": 1, "Key": "K"}]})",
    R"({"Users": [{"Name": "A", "Key": 7}]})",
    R"({"Users": [{"Name": null, "Key": "K"}]})",
    R"({"Users": [{"Name": "A", "Key": "K", "CanCreateGames": "true"}]})",
    R"({"Users": [{"Name": "A", "Key": "K", "CanCreateGames": 1}]})",
    R"({"Users": [{"Name": "A", "Key": "K", "CanCreateGames": null}]})",
    R"({"Users": [{"Name": "A", "Key": "K", "ConfirmName": "false"}]})",
    R"({"Users": [{"Name": "A", "Key": "K", "ConfirmName": 0}]})",
};

static void parse_rejects_bad_json_and_wrong_shapes() {
    using AccessRules::ParseAccessList;
    for (const char* bad : kBadUserFiles) {
        if (ParseAccessList(bad)) { std::printf("FAIL: accepted a bad user list: %s\n", bad); failures++; }
    }

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

// --- Re-reading the list while the server runs (Review Focus 2, Ruling 6) ---

using AccessRules::ApplyRead;
using AccessRules::ReloadLog;
using AccessRules::ReloadState;

static std::optional<AccessRules::AccessList> Bad() { return std::nullopt; }
static std::optional<AccessRules::AccessList> Good() { return MakeList(); }
static std::optional<AccessRules::AccessList> OtherGood() {
    return AccessRules::ParseAccessList(std::string(R"({"Users": [{"Name": "Kaz", "Key": ")") + kKazKey + R"("}]})");
}

// Who the state lets in, as Access.cpp asks it.
static bool Lets(const ReloadState& s, const char* name, const std::string& key) {
    return AccessRules::CheckJoin(s.list ? &*s.list : nullptr, s.unreadable, name, key).allowed;
}

static void reload_no_file_at_start_is_no_gate() {
    ReloadState s;
    auto o = ApplyRead(s, false, std::nullopt);
    CHECK(!o.listChanged && o.log == ReloadLog::NoFileAtStart);
    CHECK(Lets(s, "Anyone", ""));
    o = ApplyRead(s, false, std::nullopt);              // still nothing: quiet
    CHECK(!o.listChanged && o.log == ReloadLog::None);
}

static void reload_first_read_bad_refuses_everyone_and_logs_once() {
    ReloadState s;
    auto o = ApplyRead(s, true, Bad());
    CHECK(!o.listChanged && o.log == ReloadLog::BadRefuseAll);
    CHECK(s.unreadable && !s.list);
    CHECK(!Lets(s, "Sintax", kSintaxKey));
    o = ApplyRead(s, true, Bad());                      // the same bad spell: not logged again
    CHECK(!o.listChanged && o.log == ReloadLog::None);
    CHECK(!Lets(s, "Sintax", kSintaxKey));
}

static void reload_later_bad_read_keeps_the_last_good_list() {
    ReloadState s;
    auto o = ApplyRead(s, true, Good());
    CHECK(o.listChanged && o.log == ReloadLog::Loaded);
    o = ApplyRead(s, true, Bad());
    CHECK(!o.listChanged && o.log == ReloadLog::BadKeptList);
    CHECK(!s.unreadable && s.list && s.list->users.size() == 2);
    CHECK(Lets(s, "Kaz", kKazKey) && Lets(s, "Sintax", kSintaxKey));
    o = ApplyRead(s, true, Bad());                      // logged once
    CHECK(!o.listChanged && o.log == ReloadLog::None);
    CHECK(Lets(s, "Kaz", kKazKey));
}

static void reload_file_gone_after_a_good_read_keeps_the_list() {
    ReloadState s;
    ApplyRead(s, true, Good());
    auto o = ApplyRead(s, false, std::nullopt);
    CHECK(!o.listChanged && o.log == ReloadLog::GoneKeptList);
    CHECK(Lets(s, "Kaz", kKazKey));
    CHECK(!Lets(s, "Stranger", "ZZZZZZZZZZZZZZZZZZZZZZZZ"));   // still gated
    o = ApplyRead(s, false, std::nullopt);              // logged once
    CHECK(!o.listChanged && o.log == ReloadLog::None);
}

static void reload_bad_first_file_then_deleted_means_no_gate() {   // Ruling 6
    ReloadState s;
    ApplyRead(s, true, Bad());
    CHECK(!Lets(s, "Stranger", ""));
    auto o = ApplyRead(s, false, std::nullopt);
    CHECK(!o.listChanged && o.log == ReloadLog::GoneNoGate);
    CHECK(!s.unreadable && !s.list);
    CHECK(Lets(s, "Stranger", ""));
    o = ApplyRead(s, false, std::nullopt);
    CHECK(o.log == ReloadLog::None && Lets(s, "Stranger", ""));
    // A bad file coming back is a new bad spell: refused again, and logged again.
    o = ApplyRead(s, true, Bad());
    CHECK(o.log == ReloadLog::BadRefuseAll && !Lets(s, "Stranger", ""));
}

static void reload_good_read_after_bad_clears_the_problem() {
    // Bad first, then fixed.
    ReloadState s;
    ApplyRead(s, true, Bad());
    auto o = ApplyRead(s, true, Good());
    CHECK(o.listChanged && o.log == ReloadLog::Reloaded);
    CHECK(!s.unreadable && !s.problemLogged);
    CHECK(Lets(s, "Kaz", kKazKey) && !Lets(s, "Stranger", ""));
    // Good, bad, then a different good list: it applies, and a later bad spell is logged again.
    o = ApplyRead(s, true, Bad());
    CHECK(o.log == ReloadLog::BadKeptList);
    o = ApplyRead(s, true, OtherGood());
    CHECK(o.listChanged && o.log == ReloadLog::Reloaded);
    CHECK(!Lets(s, "Sintax", kSintaxKey) && Lets(s, "Kaz", kKazKey));
    o = ApplyRead(s, true, Bad());
    CHECK(o.log == ReloadLog::BadKeptList);
}

static void reload_file_back_after_removal_is_read_again() {
    ReloadState s;
    ApplyRead(s, true, Good());
    ApplyRead(s, false, std::nullopt);
    auto o = ApplyRead(s, true, OtherGood());
    CHECK(o.listChanged && o.log == ReloadLog::Reloaded);
    o = ApplyRead(s, false, std::nullopt);              // gone again: logged again
    CHECK(o.log == ReloadLog::GoneKeptList);
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
    reload_no_file_at_start_is_no_gate();
    reload_first_read_bad_refuses_everyone_and_logs_once();
    reload_later_bad_read_keeps_the_last_good_list();
    reload_file_gone_after_a_good_read_keeps_the_list();
    reload_bad_first_file_then_deleted_means_no_gate();
    reload_good_read_after_bad_clears_the_problem();
    reload_file_back_after_removal_is_read_again();

    if (failures == 0) std::printf("PASS\n");
    return failures == 0 ? 0 : 1;
}

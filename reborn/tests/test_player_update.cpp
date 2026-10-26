// reborn/tests/test_player_update.cpp
#include "../PlayerUpdate.hpp"
#include "../PlayerUpdateUi.hpp"

#include <windows.h>
#include <sddl.h>
#pragma comment(lib, "advapi32")

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

// A throwaway key pair made once with tools.signing (never the owner's key).
// kTestSig is tools.signing.sign(kTestMessage) with the key behind kTestPub.
static const char* kTestPub = "aCbn1erugwT1aWGbF91bS9ztXgickJSDCoaUdye6IfiWq/e0zU3XbBLjcK/XHgFByHzdGKmCOeGdgy6FrMqRgQ==";
static const char* kOtherPub = "C6vIzLUW+PRNAJ2tBXlJeXdAFxfsaDjIEbZ+C0JN2ofUQKFBWfOFtjRZFKPZvP+Jj5KyRHOfye7R4EzblrjNjQ==";
static const char* kTestMessage = "{\"version\":\"1.0.2\",\"build\":2}";
static const char* kTestSig = "2kerpFj2vOdZGAL7GaiIRcI2nMxFw28EqPuuDdx41IkTldQQ+CcXsfhaDbMrvLShPJe3kE5vXcHKIwMupuj80Q==";

static const std::string kHashA(64, 'a');

static std::string ManifestJson(const std::string& files) {
    return R"({"version":"1.0.2","build":2,"notes":"Fixes a crash.","files":[)" + files + "]}";
}

static std::string Entry(const std::string& name, const std::string& sha = kHashA, const std::string& size = "1024") {
    return R"({"name":")" + name + R"(","sha256":")" + sha + R"(","size":)" + size + "}";
}

static std::string ReadAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void WriteAll(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}

static bool NoLeftovers(const fs::path& dir) {
    for (const char* n : {"reborn.dll.new", "dxgi.dll.new", "reborn.dll.old", "dxgi.dll.old"})
        if (fs::exists(dir / n)) return false;
    return true;
}

// A fresh temp "Win64" folder holding the old reborn.dll and dxgi.dll.
static fs::path FreshFolder(const char* name) {
    fs::path dir = fs::temp_directory_path() / ("reborn_test_player_update_" + std::to_string(GetCurrentProcessId())) / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    WriteAll(dir / "reborn.dll", "old reborn bytes");
    WriteAll(dir / "dxgi.dll", "old dxgi bytes");
    return dir;
}

static PlayerUpdate::Manifest NewManifest(const std::string& reborn, const std::string& dxgi) {
    PlayerUpdate::Manifest m;
    m.version = "1.0.2";
    m.build = 2;
    m.notes = "test";
    m.files.push_back({"reborn.dll", PlayerUpdate::Sha256Hex(reborn), reborn.size()});
    m.files.push_back({"dxgi.dll", PlayerUpdate::Sha256Hex(dxgi), dxgi.size()});
    return m;
}

static void manifest_parses() {
    auto m = PlayerUpdate::ParseManifest(ManifestJson(Entry("reborn.dll") + "," + Entry("dxgi.dll", std::string(64, 'b'), "2048")));
    CHECK(m.has_value());
    if (!m) return;
    CHECK(m->version == "1.0.2");
    CHECK(m->build == 2);
    CHECK(m->notes == "Fixes a crash.");
    CHECK(m->files.size() == 2);
    CHECK(m->files[0].name == "reborn.dll");
    CHECK(m->files[0].sha256 == kHashA);
    CHECK(m->files[0].size == 1024);
    CHECK(m->files[1].name == "dxgi.dll");
    CHECK(m->files[1].size == 2048);

    // One file is enough; exactly 64 MiB is allowed.
    CHECK(PlayerUpdate::ParseManifest(ManifestJson(Entry("dxgi.dll", kHashA, "67108864"))).has_value());
}

static void manifest_rejects_other_names() {
    for (const char* bad : {"reborn-key.txt", "reborn-player.txt", "Reborn.dll", "REBORN.DLL", "..\\\\reborn.dll",
                            "../reborn.dll", "steam_settings\\\\reborn.dll", "reborn.pdb", "", "C:\\\\x\\\\dxgi.dll"}) {
        bool rejected = !PlayerUpdate::ParseManifest(ManifestJson(Entry(bad))).has_value();
        if (!rejected) std::printf("  accepted name: %s\n", bad);
        CHECK(rejected);
    }
    // A good name next to a bad one still rejects the whole manifest.
    CHECK(!PlayerUpdate::ParseManifest(ManifestJson(Entry("reborn.dll") + "," + Entry("reborn-key.txt"))).has_value());
}

static void manifest_rejects_bad_fields() {
    using PlayerUpdate::ParseManifest;
    CHECK(!ParseManifest("").has_value());
    CHECK(!ParseManifest("not json").has_value());
    CHECK(!ParseManifest("[]").has_value());
    CHECK(!ParseManifest(ManifestJson("")).has_value());                                   // no files
    CHECK(!ParseManifest(ManifestJson(Entry("reborn.dll") + "," + Entry("reborn.dll"))).has_value());   // repeated
    CHECK(!ParseManifest(ManifestJson(Entry("reborn.dll", kHashA, "0"))).has_value());
    CHECK(!ParseManifest(ManifestJson(Entry("reborn.dll", kHashA, "67108865"))).has_value());   // > 64 MiB
    CHECK(!ParseManifest(ManifestJson(Entry("reborn.dll", kHashA, "-5"))).has_value());
    CHECK(!ParseManifest(ManifestJson(Entry("reborn.dll", kHashA, "10.5"))).has_value());
    CHECK(!ParseManifest(ManifestJson(Entry("reborn.dll", kHashA, "\"1024\""))).has_value());
    CHECK(!ParseManifest(ManifestJson(Entry("reborn.dll", std::string(64, 'A')))).has_value());   // uppercase
    CHECK(!ParseManifest(ManifestJson(Entry("reborn.dll", std::string(63, 'a')))).has_value());
    CHECK(!ParseManifest(ManifestJson(Entry("reborn.dll", std::string(65, 'a')))).has_value());
    CHECK(!ParseManifest(ManifestJson(Entry("reborn.dll", std::string(63, 'a') + "g"))).has_value());
    CHECK(!ParseManifest(R"({"build":2,"notes":"","files":[)" + Entry("reborn.dll") + "]}").has_value());            // no version
    CHECK(!ParseManifest(R"({"version":"1.0.2","notes":"","files":[)" + Entry("reborn.dll") + "]}").has_value());    // no build
    CHECK(!ParseManifest(R"({"version":"1.0.2","build":2,"files":[)" + Entry("reborn.dll") + "]}").has_value());     // no notes
    CHECK(!ParseManifest(R"({"version":"1.0.2","build":2,"notes":""})").has_value());                                 // no files
    CHECK(!ParseManifest(R"({"version":"1.0.2","build":"2","notes":"","files":[)" + Entry("reborn.dll") + "]}").has_value());
    CHECK(!ParseManifest(R"({"version":"1.0.2","build":2.5,"notes":"","files":[)" + Entry("reborn.dll") + "]}").has_value());
    CHECK(!ParseManifest(R"({"version":"1.0.2","build":0,"notes":"","files":[)" + Entry("reborn.dll") + "]}").has_value());
    CHECK(!ParseManifest(R"({"version":"1.0.2","build":true,"notes":"","files":[)" + Entry("reborn.dll") + "]}").has_value());
    CHECK(!ParseManifest(R"({"version":102,"build":2,"notes":"","files":[)" + Entry("reborn.dll") + "]}").has_value());
    CHECK(!ParseManifest(R"({"version":"1.0.2","build":2,"notes":null,"files":[)" + Entry("reborn.dll") + "]}").has_value());
    CHECK(!ParseManifest(R"({"version":"1.0.2","build":2,"notes":"","files":{}})").has_value());
    CHECK(!ParseManifest(R"({"version":"1.0.2","build":2,"notes":"","files":["reborn.dll"]})").has_value());
    CHECK(!ParseManifest(ManifestJson(R"({"sha256":")" + kHashA + R"(","size":5})")).has_value());   // entry with no name
    // Empty notes are fine.
    CHECK(ParseManifest(R"({"version":"1.0.2","build":2,"notes":"","files":[)" + Entry("reborn.dll") + "]}").has_value());
}

static void manifest_version_matches_build_and_notes_are_short() {
    using PlayerUpdate::ParseManifest;
    auto with = [](const std::string& version, int build, const std::string& notes) {
        return R"({"version":")" + version + R"(","build":)" + std::to_string(build) + R"(,"notes":")" + notes +
               R"(","files":[)" + Entry("reborn.dll") + "]}";
    };
    CHECK(ParseManifest(with("1.0.2", 2, "")).has_value());
    CHECK(ParseManifest(with("1.0.17", 17, "")).has_value());
    CHECK(!ParseManifest(with("1.0.3", 2, "")).has_value());    // version and build disagree
    CHECK(!ParseManifest(with("1.0.02", 2, "")).has_value());
    CHECK(!ParseManifest(with("2.0.2", 2, "")).has_value());
    CHECK(!ParseManifest(with("1.0.2 ", 2, "")).has_value());
    CHECK(!ParseManifest(with("dev", 2, "")).has_value());
    CHECK(ParseManifest(with("1.0.2", 2, std::string(4096, 'n'))).has_value());
    CHECK(!ParseManifest(with("1.0.2", 2, std::string(4097, 'n'))).has_value());
}

static void signature_round_trip() {
    using PlayerUpdate::VerifySignature;
    std::string msg = kTestMessage;
    CHECK(VerifySignature(msg, kTestSig, kTestPub));
    CHECK(VerifySignature(msg, std::string(kTestSig) + "\r\n", kTestPub));   // a .sig file may end with a newline

    std::string flipped = msg;
    flipped[5] ^= 0x01;
    CHECK(!VerifySignature(flipped, kTestSig, kTestPub));

    std::string sig = kTestSig;
    sig[10] = sig[10] == 'A' ? 'B' : 'A';
    CHECK(!VerifySignature(msg, sig, kTestPub));

    CHECK(!VerifySignature(msg, kTestSig, kOtherPub));
    CHECK(!VerifySignature(msg, "", kTestPub));
    CHECK(!VerifySignature(msg, "not base64!!", kTestPub));
    CHECK(!VerifySignature(msg, kTestSig, ""));
    CHECK(!VerifySignature(msg, "AAAA", kTestPub));                 // wrong length
    CHECK(!VerifySignature(msg, kTestSig, std::string(88, 'A')));   // not a point on the curve
}

static void sha256_known_value() {
    CHECK(PlayerUpdate::Sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(PlayerUpdate::Sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    std::string binary("\0\x01\xff", 3);
    CHECK(PlayerUpdate::Sha256Hex(binary).size() == 64);
}

static void older_build_is_not_offered() {
    PlayerUpdate::Manifest m;
    m.build = 5;
    CHECK(!PlayerUpdate::ShouldOffer(m, 5));
    m.build = 4;
    CHECK(!PlayerUpdate::ShouldOffer(m, 5));
    m.build = 6;
    CHECK(PlayerUpdate::ShouldOffer(m, 5));
    for (int b : {0, 1, 6, 1000}) {
        m.build = b;
        CHECK(!PlayerUpdate::ShouldOffer(m, 0));   // a developer build never offers
    }
}

static void bad_hash_changes_nothing() {
    fs::path dir = FreshFolder("bad_hash");
    std::string reborn = "new reborn bytes", dxgi = "new dxgi bytes";
    auto m = NewManifest(reborn, dxgi);

    // dxgi.dll was damaged on the way down (same size, different bytes).
    std::map<std::string, std::string> bytes{{"reborn.dll", reborn}, {"dxgi.dll", "new dxgi bytez"}};
    auto r = PlayerUpdate::InstallFiles(dir, m, bytes);
    CHECK(!r.ok);
    CHECK(!r.message.empty());
    CHECK(ReadAll(dir / "reborn.dll") == "old reborn bytes");
    CHECK(ReadAll(dir / "dxgi.dll") == "old dxgi bytes");
    CHECK(NoLeftovers(dir));

    // Wrong size.
    bytes = {{"reborn.dll", reborn + "x"}, {"dxgi.dll", dxgi}};
    r = PlayerUpdate::InstallFiles(dir, m, bytes);
    CHECK(!r.ok);
    CHECK(ReadAll(dir / "reborn.dll") == "old reborn bytes");
    CHECK(NoLeftovers(dir));

    // A file that was never downloaded.
    bytes = {{"reborn.dll", reborn}};
    r = PlayerUpdate::InstallFiles(dir, m, bytes);
    CHECK(!r.ok);
    CHECK(ReadAll(dir / "reborn.dll") == "old reborn bytes");
    CHECK(ReadAll(dir / "dxgi.dll") == "old dxgi bytes");
    CHECK(NoLeftovers(dir));

    // A hand-built manifest with a name outside the allowed two is refused.
    PlayerUpdate::Manifest evil;
    evil.version = "1.0.2";
    evil.build = 2;
    evil.files.push_back({"reborn-key.txt", PlayerUpdate::Sha256Hex("x"), 1});
    r = PlayerUpdate::InstallFiles(dir, evil, {{"reborn-key.txt", "x"}});
    CHECK(!r.ok);
    CHECK(!fs::exists(dir / "reborn-key.txt"));
    CHECK(!fs::exists(dir / "reborn-key.txt.new"));
}

static void install_swaps_both_files() {
    fs::path dir = FreshFolder("swap");
    WriteAll(dir / "reborn.dll.old", "stale old file from an earlier update");
    std::string reborn = "new reborn bytes", dxgi = "new dxgi bytes";
    auto r = PlayerUpdate::InstallFiles(dir, NewManifest(reborn, dxgi), {{"reborn.dll", reborn}, {"dxgi.dll", dxgi}});
    CHECK(r.ok);
    CHECK(ReadAll(dir / "reborn.dll") == reborn);
    CHECK(ReadAll(dir / "dxgi.dll") == dxgi);
    CHECK(ReadAll(dir / "reborn.dll.old") == "old reborn bytes");
    CHECK(ReadAll(dir / "dxgi.dll.old") == "old dxgi bytes");
    CHECK(!fs::exists(dir / "reborn.dll.new"));
    CHECK(!fs::exists(dir / "dxgi.dll.new"));
}

static void rename_failure_rolls_back() {
    fs::path dir = FreshFolder("rollback");
    // Hold dxgi.dll open without share-delete (like antivirus), so it cannot be renamed.
    HANDLE h = CreateFileW((dir / "dxgi.dll").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);

    std::string reborn = "new reborn bytes", dxgi = "new dxgi bytes";
    auto r = PlayerUpdate::InstallFiles(dir, NewManifest(reborn, dxgi), {{"reborn.dll", reborn}, {"dxgi.dll", dxgi}});
    CloseHandle(h);

    CHECK(!r.ok);
    CHECK(r.message.find("in use") != std::string::npos);
    CHECK(ReadAll(dir / "reborn.dll") == "old reborn bytes");
    CHECK(ReadAll(dir / "dxgi.dll") == "old dxgi bytes");
    CHECK(NoLeftovers(dir));
}

static void second_rename_failure_rolls_back() {
    fs::path dir = FreshFolder("rollback_second");
    // dxgi.dll moves to dxgi.dll.old fine, but dxgi.dll.new cannot be renamed into place.
    WriteAll(dir / "dxgi.dll.new", "");
    HANDLE h = CreateFileW((dir / "dxgi.dll.new").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);

    std::string reborn = "new reborn bytes", dxgi = "new dxgi bytes";
    auto r = PlayerUpdate::InstallFiles(dir, NewManifest(reborn, dxgi), {{"reborn.dll", reborn}, {"dxgi.dll", dxgi}});
    CloseHandle(h);

    CHECK(!r.ok);
    CHECK(r.message.find("in use") != std::string::npos);
    CHECK(ReadAll(dir / "reborn.dll") == "old reborn bytes");
    CHECK(ReadAll(dir / "dxgi.dll") == "old dxgi bytes");
    CHECK(!fs::exists(dir / "reborn.dll.old"));
    CHECK(!fs::exists(dir / "dxgi.dll.old"));
    CHECK(!fs::exists(dir / "reborn.dll.new"));
    // dxgi.dll.new could not be deleted while it was held open; the next start's CleanupOld removes it.
    PlayerUpdate::CleanupOld(dir);
    CHECK(NoLeftovers(dir));
}

static bool SetDacl(const fs::path& p, const wchar_t* sddl) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr)) return false;
    BOOL ok = SetFileSecurityW(p.c_str(), DACL_SECURITY_INFORMATION, sd);
    LocalFree(sd);
    return ok != 0;
}

static void unreadable_file_is_a_failure_not_missing() {
    fs::path dir = FreshFolder("unreadable");
    // Nobody may read reborn.dll's attributes (or list the folder, which would grant that), so checking
    // whether reborn.dll exists fails with an error. That must stop the install, not count as "missing"
    // and let reborn.dll.new overwrite a file that was never backed up.
    CHECK(SetDacl(dir / "reborn.dll", L"D:P(D;;0x80;;;WD)(A;;FA;;;WD)"));
    CHECK(SetDacl(dir, L"D:P(D;;0x1;;;WD)(A;OICI;FA;;;WD)"));
    std::error_code probe;
    fs::exists(dir / "reborn.dll", probe);
    CHECK(probe);   // the setup really makes the existence check fail

    std::string reborn = "new reborn bytes", dxgi = "new dxgi bytes";
    auto r = PlayerUpdate::InstallFiles(dir, NewManifest(reborn, dxgi), {{"reborn.dll", reborn}, {"dxgi.dll", dxgi}});

    CHECK(SetDacl(dir, L"D:P(A;OICI;FA;;;WD)"));
    CHECK(SetDacl(dir / "reborn.dll", L"D:P(A;;FA;;;WD)"));
    CHECK(!r.ok);
    CHECK(ReadAll(dir / "reborn.dll") == "old reborn bytes");
    CHECK(ReadAll(dir / "dxgi.dll") == "old dxgi bytes");
    CHECK(NoLeftovers(dir));
}

static void cleanup_removes_old_files() {
    fs::path dir = FreshFolder("cleanup");
    for (const char* n : {"reborn.dll.old", "dxgi.dll.old", "reborn.dll.bad", "reborn.dll.new", "dxgi.dll.new"})
        WriteAll(dir / n, "x");
    PlayerUpdate::CleanupOld(dir);
    CHECK(!fs::exists(dir / "reborn.dll.old"));
    CHECK(!fs::exists(dir / "dxgi.dll.old"));
    CHECK(!fs::exists(dir / "reborn.dll.bad"));
    CHECK(!fs::exists(dir / "reborn.dll.new"));
    CHECK(!fs::exists(dir / "dxgi.dll.new"));
    CHECK(ReadAll(dir / "reborn.dll") == "old reborn bytes");
    CHECK(ReadAll(dir / "dxgi.dll") == "old dxgi bytes");
    PlayerUpdate::CleanupOld(dir);                       // nothing there: still fine
    PlayerUpdate::CleanupOld(dir / "does-not-exist");    // missing folder: still fine
}

// The notice's words, built from what InstallFiles really says.
static void notice_texts_read_plainly() {
    using namespace PlayerUpdateUi;
    CHECK(OfferText("1.0.3") == "Update available: Reborn 1.0.3");
    CHECK(std::string(kReadyText) == "Update ready. Restart the game to use it.");
    CHECK(FailureText("the download did not finish") ==
          "The update could not be installed: the download did not finish. You can keep playing.");

    fs::path dir = FreshFolder("texts");
    std::string reborn = "new reborn bytes", dxgi = "new dxgi bytes";
    auto damaged = PlayerUpdate::InstallFiles(dir, NewManifest(reborn, dxgi), {{"reborn.dll", reborn}, {"dxgi.dll", "new dxgi bytez"}});
    CHECK(!damaged.ok);
    CHECK(InstallFailureReason(damaged.message) == "the download was damaged");

    HANDLE h = CreateFileW((dir / "dxgi.dll").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    auto inUse = PlayerUpdate::InstallFiles(dir, NewManifest(reborn, dxgi), {{"reborn.dll", reborn}, {"dxgi.dll", dxgi}});
    CloseHandle(h);
    CHECK(!inUse.ok);
    CHECK(FailureText(InstallFailureReason(inUse.message)) ==
          "The update could not be installed: a game file is in use. Please try again after restarting the game. "
          "You can keep playing.");

    CHECK(InstallFailureReason("The update could not be installed, and the old files could not all be put back. "
                               "Please reinstall Battleborn Reborn.") ==
          "the old files could not all be put back. Please reinstall Battleborn Reborn");
    CHECK(InstallFailureReason("The update could not be installed. Nothing was changed.") == "the new files could not be saved");
    CHECK(InstallFailureReason("") == "the new files could not be saved");
}

// Who checks: only a released player build that was not told not to.
static void only_released_players_check() {
    using PlayerUpdateUi::WhyNoCheck;
    CHECK(WhyNoCheck(false, false, false, 3).empty());
    CHECK(!WhyNoCheck(true, false, false, 3).empty());    // a game server
    CHECK(!WhyNoCheck(false, true, false, 3).empty());    // -rbautopilot
    CHECK(!WhyNoCheck(false, false, true, 3).empty());    // -rbnoupdate
    CHECK(!WhyNoCheck(false, false, false, 0).empty());   // a developer build
    CHECK(PlayerUpdateUi::kMaxManifestResponse == 64 * 1024);
}

int main() {
    manifest_parses();
    manifest_rejects_other_names();
    manifest_rejects_bad_fields();
    signature_round_trip();
    sha256_known_value();
    older_build_is_not_offered();
    bad_hash_changes_nothing();
    install_swaps_both_files();
    rename_failure_rolls_back();
    second_rename_failure_rolls_back();
    unreadable_file_is_a_failure_not_missing();
    manifest_version_matches_build_and_notes_are_short();
    cleanup_removes_old_files();
    notice_texts_read_plainly();
    only_released_players_check();

    std::error_code ec;
    fs::remove_all(fs::temp_directory_path() / ("reborn_test_player_update_" + std::to_string(GetCurrentProcessId())), ec);

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}

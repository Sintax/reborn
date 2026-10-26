// reborn/tests/test_mod_rollback.cpp: dxgi's "the new mod will not load" rollback.
#include "../../dxgi/ModRollback.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static fs::path Root() {
    return fs::temp_directory_path() / ("reborn_test_mod_rollback_" + std::to_string(GetCurrentProcessId()));
}

static fs::path Fresh(const char* name) {
    fs::path dir = Root() / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

static void WriteAll(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}

static std::string ReadAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void puts_the_previous_mod_back() {
    fs::path dir = Fresh("back");
    WriteAll(dir / "reborn.dll", "new, broken");
    WriteAll(dir / "reborn.dll.old", "previous");
    WriteAll(dir / "reborn.dll.bad", "from an earlier rollback");
    CHECK(ModRollback::PutBackPreviousMod(dir.wstring()));
    CHECK(ReadAll(dir / "reborn.dll") == "previous");
    CHECK(ReadAll(dir / "reborn.dll.bad") == "new, broken");
    CHECK(!fs::exists(dir / "reborn.dll.old"));
}

static void missing_mod_with_old_one() {
    fs::path dir = Fresh("missing");
    WriteAll(dir / "reborn.dll.old", "previous");
    CHECK(ModRollback::PutBackPreviousMod(dir.wstring()));
    CHECK(ReadAll(dir / "reborn.dll") == "previous");
    CHECK(!fs::exists(dir / "reborn.dll.bad"));
}

static void nothing_to_put_back() {
    fs::path dir = Fresh("none");
    WriteAll(dir / "reborn.dll", "only one");
    CHECK(!ModRollback::PutBackPreviousMod(dir.wstring()));
    CHECK(ReadAll(dir / "reborn.dll") == "only one");
    CHECK(!fs::exists(dir / "reborn.dll.bad"));
}

static void locked_old_file_leaves_the_mod_in_place() {
    fs::path dir = Fresh("locked");
    WriteAll(dir / "reborn.dll", "new, broken");
    WriteAll(dir / "reborn.dll.old", "previous");
    // reborn.dll.old cannot be renamed (held open without share-delete, like antivirus).
    HANDLE h = CreateFileW((dir / "reborn.dll.old").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    CHECK(!ModRollback::PutBackPreviousMod(dir.wstring()));
    CloseHandle(h);
    CHECK(ReadAll(dir / "reborn.dll") == "new, broken");
    CHECK(ReadAll(dir / "reborn.dll.old") == "previous");
    CHECK(!fs::exists(dir / "reborn.dll.bad"));
}

int main() {
    puts_the_previous_mod_back();
    missing_mod_with_old_one();
    nothing_to_put_back();
    locked_old_file_leaves_the_mod_in_place();

    std::error_code ec;
    fs::remove_all(Root(), ec);
    std::puts(failures ? "FAIL" : "PASS");
    return failures;
}

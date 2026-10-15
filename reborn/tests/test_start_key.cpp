// reborn/tests/test_start_key.cpp
#include "../StartKey.hpp"
#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

int main() {
    CHECK(StartKey::TrimKey("ABCDEFGHJKMNPQRSTVWXYZ23") == "ABCDEFGHJKMNPQRSTVWXYZ23");
    CHECK(StartKey::TrimKey("ABCDEFGHJKMNPQRSTVWXYZ23\r\n") == "ABCDEFGHJKMNPQRSTVWXYZ23");
    CHECK(StartKey::TrimKey("\xEF\xBB\xBF" "ABCDEFGHJKMNPQRSTVWXYZ23\n") == "ABCDEFGHJKMNPQRSTVWXYZ23");   // UTF-8 BOM from Notepad
    CHECK(StartKey::TrimKey("  ABC  \r\n\r\n") == "ABC");
    CHECK(StartKey::TrimKey("\r\n") == "");
    CHECK(StartKey::TrimKey("") == "");

    CHECK(StartKey::DefaultGameName("DJ", "Story - The Algorithm") == "DJ's The Algorithm");
    CHECK(StartKey::DefaultGameName("DJ", "Incursion - Overgrowth") == "DJ's Overgrowth");
    CHECK(StartKey::DefaultGameName("", "Story - The Algorithm") == "The Algorithm");
    CHECK(StartKey::DefaultGameName("DJ", "Weird") == "DJ's Weird");
    CHECK(StartKey::DefaultGameName("A very long player name indeed yes", "Operations - Phoebe and the Heart of Ekkunar").size() == 40);

    std::printf(failures ? "FAILED %d\n" : "OK\n", failures);
    return failures ? 1 : 0;
}

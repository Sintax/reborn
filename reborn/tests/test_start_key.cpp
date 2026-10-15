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

    // Clipping must not slice a multi-byte UTF-8 character: "a" + 25 x "e-acute" puts byte 40 mid-character.
    {
        std::string player = "a";
        for (int i = 0; i < 25; i++) player += "\xC3" "\xA9";
        std::string r = StartKey::DefaultGameName(player, "Story - X");
        CHECK(!r.empty() && r.size() <= 40);
        CHECK((unsigned char)r.back() != 0xC3);
        // every lead byte must be followed by its continuation byte(s)
        bool valid = true;
        for (size_t i = 0; i < r.size();) {
            unsigned char c = (unsigned char)r[i];
            size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
            if (len == 0 || i + len > r.size()) { valid = false; break; }
            for (size_t k = 1; k < len; k++) if (((unsigned char)r[i + k] & 0xC0) != 0x80) valid = false;
            i += len;
        }
        CHECK(valid);
    }

    std::printf(failures ? "FAILED %d\n" : "OK\n", failures);
    return failures ? 1 : 0;
}

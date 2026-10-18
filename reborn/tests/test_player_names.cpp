// reborn/tests/test_player_names.cpp
#include "../PlayerNameRules.hpp"
#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

int main() {
    using namespace PlayerNames;

    CHECK(CleanName("DJ_Sintax-2") == "DJ_Sintax-2");
    CHECK(CleanName("  Ann  \r\n") == "Ann");
    CHECK(CleanName("a b?c=d#e,f<g>h&i") == "abcdefghi");
    CHECK(CleanName("") == "Player");
    CHECK(CleanName("   ") == "Player");
    CHECK(CleanName("???") == "Player");
    CHECK(CleanName("\xC3\xA9t\xC3\xA9") == "t");            // UTF-8 letters are dropped
    CHECK(CleanName(std::string(40, 'x')) == std::string(24, 'x'));
    CHECK(CleanName("??" + std::string(30, 'y')).size() == 24);   // the cut counts kept characters

    CHECK(UniqueName("Ann", {}) == "Ann");
    CHECK(UniqueName("Ann", { "Bob" }) == "Ann");
    CHECK(UniqueName("Ann", { "Ann" }) == "Ann-2");
    CHECK(UniqueName("Ann", { "ann" }) == "Ann-2");          // case does not tell players apart
    CHECK(UniqueName("Ann", { "Ann", "Ann-2" }) == "Ann-3");
    CHECK(UniqueName("Ann", { "Ann-2" }) == "Ann");
    CHECK(UniqueName("Player", { "Player", "Player-2", "Player-3" }) == "Player-4");
    // A de-duplicated name still follows CleanName's rule: at most 24 characters, no spaces.
    {
        std::string full(24, 'x');
        std::string u = UniqueName(full, { full });
        CHECK(u.size() == 24 && u == std::string(22, 'x') + "-2");
        CHECK(CleanName(u) == u);
    }

    // Client text in the log: no line breaks or control characters, cut to 40.
    CHECK(SafeLogText("Ann") == "Ann");
    CHECK(SafeLogText("x\n[TEAMS] server: fake") == "x?[TEAMS] server: fake");
    CHECK(SafeLogText("a\rb\tc\x01" "d") == "a?b?c?d");
    CHECK(SafeLogText("\xC3\xA9") == "??");
    CHECK(SafeLogText(std::string(100, 'y')) == std::string(40, 'y') + "...");
    CHECK(SafeLogText(std::string(40, 'y')) == std::string(40, 'y'));
    CHECK(ToAscii(L"Ann\n\x00e9") == "Ann??");

    if (failures == 0) std::printf("PASS\n");
    return failures == 0 ? 0 : 1;
}

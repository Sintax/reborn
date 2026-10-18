#pragma once

#include <cctype>
#include <string>
#include <vector>

// The string-only part of PlayerNames, kept free of the SDK so the unit tests can build it.
namespace PlayerNames {
    constexpr size_t kMaxNameLength = 24;

    // A name as players may show it: only A-Z a-z 0-9 _ - (the installer's rule), at most 24
    // characters, "Player" when nothing is left. Keeps out ? = # and spaces (URLs, console
    // commands), commas (the [TEAMS] list) and < > & (the HTML copies of the name).
    inline std::string CleanName(const std::string& wanted) {
        std::string out;
        for (char c : wanted) {
            unsigned char u = (unsigned char)c;
            if (u < 0x80 && (std::isalnum(u) || c == '_' || c == '-')) out += c;
            if (out.size() == kMaxNameLength) break;
        }
        return out.empty() ? std::string("Player") : out;
    }

    inline bool SameName(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++)
            if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
        return true;
    }

    // `name`, or `name 2`, `name 3`, ... when another player already has it (ignoring case).
    inline std::string UniqueName(const std::string& name, const std::vector<std::string>& taken) {
        auto isTaken = [&](const std::string& n) {
            for (const std::string& t : taken) if (SameName(t, n)) return true;
            return false;
        };
        if (!isTaken(name)) return name;
        for (int i = 2;; i++) {
            std::string candidate = name + " " + std::to_string(i);
            if (!isTaken(candidate)) return candidate;
        }
    }
}

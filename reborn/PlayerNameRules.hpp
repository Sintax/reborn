#pragma once

#include <cctype>
#include <cstddef>
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

    // `name`, or `name-2`, `name-3`, ... when another player already has it (ignoring case). The
    // name is cut so the result stays within 24 characters and keeps CleanName's characters.
    inline std::string UniqueName(const std::string& name, const std::vector<std::string>& taken) {
        auto isTaken = [&](const std::string& n) {
            for (const std::string& t : taken) if (SameName(t, n)) return true;
            return false;
        };
        if (!isTaken(name)) return name;
        for (int i = 2;; i++) {
            std::string suffix = "-" + std::to_string(i);
            std::string candidate = name.substr(0, kMaxNameLength - suffix.size()) + suffix;
            if (!isTaken(candidate)) return candidate;
        }
    }

    // Wide text from a client as plain ASCII; anything else becomes '?'.
    inline std::string ToAscii(const std::wstring& text) {
        std::string out;
        for (wchar_t c : text) out += (c >= 0x20 && c < 0x7f) ? (char)c : '?';
        return out;
    }

    // Client text made safe for one log line: control and non-ASCII characters become '?' (so it
    // cannot start a fake line), at most `max` characters, "..." when cut.
    inline std::string SafeLogText(const std::string& text, size_t max = 40) {
        std::string out;
        for (char c : text) {
            if (out.size() == max) return out + "...";
            unsigned char u = (unsigned char)c;
            out += (u >= 0x20 && u < 0x7f) ? c : '?';
        }
        return out;
    }
}

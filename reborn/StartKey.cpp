// reborn/StartKey.cpp
#include "StartKey.hpp"

#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#endif

namespace StartKey {
    namespace {
        const char* kKeyFile = "reborn-key.txt";
        const char* kNameFile = "reborn-player.txt";
        std::mutex g_lock;
        std::optional<std::string> g_key, g_name;

        std::string ReadOneLine(const std::filesystem::path& p) {
            std::ifstream f(p, std::ios::binary);
            if (!f) return "";
            std::stringstream ss;
            ss << f.rdbuf();
            return TrimKey(ss.str());
        }
    }

    std::string TrimKey(const std::string& text) {
        std::string s = text;
        if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s.erase(0, 3);
        const char* ws = " \t\r\n";
        size_t a = s.find_first_not_of(ws);
        if (a == std::string::npos) return "";
        size_t b = s.find_last_not_of(ws);
        return s.substr(a, b - a + 1);
    }

    std::string DefaultGameName(const std::string& player, const std::string& mapMode) {
        size_t dash = mapMode.find(" - ");
        std::string map = dash == std::string::npos ? mapMode : mapMode.substr(dash + 3);
        std::string name = player.empty() ? map : player + "'s " + map;
        if (name.size() > 40) {
            // Clip to 40 bytes, but never in the middle of a UTF-8 character: back up past
            // continuation bytes (10xxxxxx) so the cut lands on a character boundary.
            size_t cut = 40;
            while (cut > 0 && ((unsigned char)name[cut] & 0xC0) == 0x80) cut--;
            name.resize(cut);
        }
        return name;
    }

    std::filesystem::path Win64Dir() {
#ifdef _WIN32
        std::wstring buf(MAX_PATH, L'\0');
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        buf.resize(n);
        return std::filesystem::path(buf).parent_path();
#else
        return std::filesystem::current_path();
#endif
    }

    std::string Key() {
        std::scoped_lock l(g_lock);
        if (!g_key) g_key = ReadOneLine(Win64Dir() / kKeyFile);
        return *g_key;
    }

    std::string PlayerName() {
        std::scoped_lock l(g_lock);
        if (!g_name) g_name = ReadOneLine(Win64Dir() / kNameFile);
        return *g_name;
    }

    void SavePlayerName(const std::string& name) {
        std::scoped_lock l(g_lock);
        g_name = TrimKey(name);
        std::ofstream f(Win64Dir() / kNameFile, std::ios::binary | std::ios::trunc);
        f << *g_name << "\r\n";
    }
}

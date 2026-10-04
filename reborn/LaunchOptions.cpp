#include "LaunchOptions.hpp"
#include <Windows.h>
#include <shellapi.h>
#include <climits>
#pragma comment(lib, "shell32.lib")

namespace LaunchOptions {
    static Options g_options;

    static std::string Narrow(const std::wstring& w) {
        if (w.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string s(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
        return s;
    }

    static bool ToInt(const std::wstring& v, int lo, int hi, int& out) {
        try {
            size_t used = 0;
            int n = std::stoi(v, &used);
            if (used != v.size() || n < lo || n > hi) return false;
            out = n;
            return true;
        }
        catch (...) {
            return false;
        }
    }

    Options Parse(const std::wstring& commandLine) {
        Options o;
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(commandLine.c_str(), &argc);
        if (!argv) return o;
        for (int i = 1; i < argc; i++) {
            std::wstring a = argv[i];
            if (a.rfind(L"-rb", 0) != 0) continue;
            std::wstring key = a, val;
            size_t eq = a.find(L'=');
            if (eq != std::wstring::npos) {
                key = a.substr(0, eq);
                val = a.substr(eq + 1);
            }
            int n = 0;
            if (key == L"-rbinstance" && !val.empty()) o.instance = val;
            else if (key == L"-rbdebugport") { if (ToInt(val, 1024, 65535, n)) o.debugPort = n; else o.errors.push_back(a); }
            else if (key == L"-rbrundir" && !val.empty()) o.runDir = val;
            else if (key == L"-rbautopilot") o.autopilot = true;
            else if (key == L"-rbjoin" && !val.empty()) o.join = val;
            else if (key == L"-rbsolomap" && !val.empty()) o.soloMap = val;
            else if (key == L"-rbservermap" && !val.empty()) o.serverMap = val;
            else if (key == L"-rbplayers") { if (ToInt(val, 1, 10, n)) o.players = n; else o.errors.push_back(a); }
            else if (key == L"-rbcharacter" && !val.empty()) o.character = Narrow(val);
            else if (key == L"-rbseed") { if (ToInt(val, 0, INT_MAX, n)) o.seed = (unsigned)n; else o.errors.push_back(a); }
            else if (key == L"-rbhangsecs") { if (ToInt(val, 5, 3600, n)) o.hangSeconds = n; else o.errors.push_back(a); }
            else if (key == L"-rbtesthang") o.testHang = true;
            else if (key == L"-rbtestcrash") o.testCrash = true;
            else o.unknown.push_back(a);
        }
        LocalFree(argv);
        return o;
    }

    void InitFromCommandLine() { g_options = Parse(GetCommandLineW()); }
    const Options& Get() { return g_options; }
}

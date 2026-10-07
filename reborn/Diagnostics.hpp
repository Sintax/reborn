#pragma once
#include <string>
#include <vector>

namespace Diagnostics {
    void Init(const std::wstring& runDir, const std::wstring& instance, int hangSeconds);
    // Where the engine writes its fatal-error text before raising 0xDEAD; attached to that crash report.
    void SetFatalErrorBuffer(const wchar_t* buffer, size_t maxChars);
    void NoteTick();
    long long MillisSinceLastTick();
    unsigned long long TickCount();
    std::vector<std::string> RecentLines();
    std::string LastLineContaining(const std::string& needle);
    std::string FormatAddress(unsigned long long address);
    const std::wstring& RunDir();
}

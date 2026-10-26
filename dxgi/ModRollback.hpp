#pragma once

#include <windows.h>
#include <string>

// After a player update, the previous mod is kept as reborn.dll.old until the next start.
// If the new reborn.dll will not load, put the previous one back:
//   reborn.dll -> reborn.dll.bad, reborn.dll.old -> reborn.dll.
// Returns true when reborn.dll is the previous mod again (the caller then loads it).
// Never throws; any failed step leaves reborn.dll as it was.
namespace ModRollback {

inline bool Exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

inline bool PutBackPreviousMod(const std::wstring& win64) {
    try {
        const std::wstring cur = win64 + L"\\reborn.dll";
        const std::wstring old = cur + L".old";
        const std::wstring bad = cur + L".bad";
        if (!Exists(old)) return false;
        const bool hadCur = Exists(cur);
        if (hadCur && !MoveFileExW(cur.c_str(), bad.c_str(), MOVEFILE_REPLACE_EXISTING)) return false;
        if (MoveFileExW(old.c_str(), cur.c_str(), 0)) return true;
        if (hadCur) MoveFileExW(bad.c_str(), cur.c_str(), 0);   // put the new one back where it was
        return false;
    } catch (...) {
        return false;
    }
}

}  // namespace ModRollback

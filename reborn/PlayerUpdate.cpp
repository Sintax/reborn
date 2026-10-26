// reborn/PlayerUpdate.cpp
// Pure player-update logic: manifest parsing, signature and hash checks, the file swap.
// No game or UE3 headers here, so test_player_update.cpp can build it on its own.
#include "PlayerUpdate.hpp"

#include "json.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <cstring>
#include <fstream>
#include <set>

#pragma comment(lib, "bcrypt")

namespace PlayerUpdate {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

bool IsAllowedName(const std::string& name) {
    return name == "reborn.dll" || name == "dxgi.dll";
}

bool IsLowerHex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

std::string_view TrimAscii(std::string_view s) {
    auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && ws(s.front())) s.remove_prefix(1);
    while (!s.empty() && ws(s.back())) s.remove_suffix(1);
    return s;
}

// Strict standard base64 (with padding). Returns nothing on any malformed input.
std::optional<std::string> Base64Decode(std::string_view in) {
    in = TrimAscii(in);
    if (in.empty() || in.size() % 4 != 0) return std::nullopt;
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    out.reserve(in.size() / 4 * 3);
    for (size_t i = 0; i < in.size(); i += 4) {
        bool last = i + 4 == in.size();
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; k++) {
            char c = in[i + k];
            if (c == '=') {
                // Padding only in the last group, only in the last two places, and only at the end.
                if (!last || k < 2) return std::nullopt;
                pad++;
                v[k] = 0;
            } else {
                if (pad) return std::nullopt;
                v[k] = value(c);
                if (v[k] < 0) return std::nullopt;
            }
        }
        uint32_t n = (uint32_t(v[0]) << 18) | (uint32_t(v[1]) << 12) | (uint32_t(v[2]) << 6) | uint32_t(v[3]);
        out.push_back(char((n >> 16) & 0xFF));
        if (pad < 2) out.push_back(char((n >> 8) & 0xFF));
        if (pad < 1) out.push_back(char(n & 0xFF));
    }
    return out;
}

// SHA-256 into out[32]; false if the crypto API fails.
bool Sha256Raw(std::string_view bytes, unsigned char out[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) {
        ok = true;
        const unsigned char* p = reinterpret_cast<const unsigned char*>(bytes.data());
        size_t left = bytes.size();
        while (ok && left > 0) {
            ULONG chunk = left > 0x40000000 ? 0x40000000 : ULONG(left);
            ok = BCRYPT_SUCCESS(BCryptHashData(hash, const_cast<PUCHAR>(p), chunk, 0));
            p += chunk;
            left -= chunk;
        }
        ok = ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, out, 32, 0));
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

bool Rename(const fs::path& from, const fs::path& to) {
    return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

void RemoveQuietly(const fs::path& p) {
    std::error_code ec;
    fs::remove(p, ec);
}

bool WriteFileBytes(const fs::path& p, const std::string& bytes) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(bytes.data(), std::streamsize(bytes.size()));
    f.close();
    return !f.fail();
}

fs::path WithSuffix(const fs::path& dir, const std::string& name, const char* suffix) {
    return dir / (name + suffix);
}

constexpr const char* kNothingChanged = "The update could not be installed. Nothing was changed.";
constexpr const char* kDamaged = "The update download was damaged. Nothing was changed. Please try again later.";

}  // namespace

std::optional<Manifest> ParseManifest(std::string_view text) {
    try {
        json j = json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
        if (j.is_discarded() || !j.is_object()) return std::nullopt;

        auto version = j.find("version");
        auto build = j.find("build");
        auto notes = j.find("notes");
        auto files = j.find("files");
        if (version == j.end() || !version->is_string()) return std::nullopt;
        if (build == j.end() || !build->is_number_integer()) return std::nullopt;   // excludes floats and bools
        if (notes == j.end() || !notes->is_string()) return std::nullopt;
        if (files == j.end() || !files->is_array() || files->empty()) return std::nullopt;

        Manifest m;
        m.version = version->get<std::string>();
        if (m.version.empty()) return std::nullopt;
        int64_t b = build->get<int64_t>();
        if (b < 1 || b > INT32_MAX) return std::nullopt;
        m.build = int(b);
        m.notes = notes->get<std::string>();

        std::set<std::string> seen;
        for (const json& f : *files) {
            if (!f.is_object()) return std::nullopt;
            auto name = f.find("name");
            auto sha = f.find("sha256");
            auto size = f.find("size");
            if (name == f.end() || !name->is_string()) return std::nullopt;
            if (sha == f.end() || !sha->is_string()) return std::nullopt;
            if (size == f.end() || !size->is_number_integer()) return std::nullopt;

            FileEntry e;
            e.name = name->get<std::string>();
            e.sha256 = sha->get<std::string>();
            if (!IsAllowedName(e.name) || !seen.insert(e.name).second) return std::nullopt;
            if (!IsLowerHex64(e.sha256)) return std::nullopt;
            if (size->is_number_unsigned()) {
                e.size = size->get<uint64_t>();
            } else {
                int64_t s = size->get<int64_t>();
                if (s < 0) return std::nullopt;
                e.size = uint64_t(s);
            }
            if (e.size == 0 || e.size > kMaxFileSize) return std::nullopt;
            m.files.push_back(std::move(e));
        }
        return m;
    } catch (...) {
        return std::nullopt;
    }
}

bool VerifySignature(std::string_view data, std::string_view sigB64, std::string_view pubB64) {
    std::optional<std::string> sig = Base64Decode(sigB64);
    std::optional<std::string> pub = Base64Decode(pubB64);
    if (!sig || !pub || sig->size() != 64 || pub->size() != 64) return false;

    unsigned char digest[32];
    if (!Sha256Raw(data, digest)) return false;

    // BCRYPT_ECCPUBLIC_BLOB: header, then X, then Y (32 bytes each, big-endian).
    unsigned char blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
    BCRYPT_ECCKEY_BLOB header{BCRYPT_ECDSA_PUBLIC_P256_MAGIC, 32};
    memcpy(blob, &header, sizeof(header));
    memcpy(blob + sizeof(header), pub->data(), 64);

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    bool ok = false;
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, blob, ULONG(sizeof(blob)), 0))) {
        ok = BCRYPT_SUCCESS(BCryptVerifySignature(key, nullptr, digest, 32,
                                                  reinterpret_cast<PUCHAR>(sig->data()), 64, 0));
    }
    if (key) BCryptDestroyKey(key);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

std::string Sha256Hex(std::string_view bytes) {
    unsigned char digest[32];
    if (!Sha256Raw(bytes, digest)) return "";
    static const char* hex = "0123456789abcdef";
    std::string out(64, '0');
    for (int i = 0; i < 32; i++) {
        out[2 * i] = hex[digest[i] >> 4];
        out[2 * i + 1] = hex[digest[i] & 0xF];
    }
    return out;
}

bool ShouldOffer(const Manifest& m, int currentBuild) {
    return currentBuild > 0 && m.build > currentBuild;
}

InstallOutcome InstallFiles(const fs::path& win64, const Manifest& m,
                            const std::map<std::string, std::string>& bytesByName) {
    try {
        // 1. Check everything before touching the folder.
        if (m.files.empty()) return {false, kNothingChanged};
        std::set<std::string> seen;
        for (const FileEntry& f : m.files) {
            if (!IsAllowedName(f.name) || !seen.insert(f.name).second) return {false, kNothingChanged};
            auto it = bytesByName.find(f.name);
            if (it == bytesByName.end()) return {false, kDamaged};
            if (it->second.size() != f.size || Sha256Hex(it->second) != f.sha256) return {false, kDamaged};
        }

        // 2. Write every <name>.new.
        auto removeNew = [&] {
            for (const FileEntry& f : m.files) RemoveQuietly(WithSuffix(win64, f.name, ".new"));
        };
        for (const FileEntry& f : m.files) {
            if (!WriteFileBytes(WithSuffix(win64, f.name, ".new"), bytesByName.at(f.name))) {
                removeNew();
                return {false, kNothingChanged};
            }
        }

        // 3. Swap each file: <name> -> <name>.old, <name>.new -> <name>.
        struct Step { const FileEntry* file; bool movedOld; bool placedNew; };
        std::vector<Step> steps;
        steps.reserve(m.files.size());   // no allocation once files start moving
        bool failed = false;
        for (const FileEntry& f : m.files) {
            fs::path cur = win64 / f.name;
            Step s{&f, false, false};
            std::error_code ec;
            if (fs::exists(cur, ec)) {
                if (!Rename(cur, WithSuffix(win64, f.name, ".old"))) { failed = true; steps.push_back(s); break; }
                s.movedOld = true;
            }
            if (!Rename(WithSuffix(win64, f.name, ".new"), cur)) { failed = true; steps.push_back(s); break; }
            s.placedNew = true;
            steps.push_back(s);
        }
        if (!failed) return {true, ""};

        // 4. Put back what was already swapped, newest first.
        bool restored = true;
        for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
            fs::path cur = win64 / it->file->name;
            if (it->movedOld) {
                if (!Rename(WithSuffix(win64, it->file->name, ".old"), cur)) restored = false;
            } else if (it->placedNew) {
                std::error_code ec;
                if (!fs::remove(cur, ec)) restored = false;
            }
        }
        removeNew();
        if (!restored)
            return {false, "The update could not be installed, and the old files could not all be put back. "
                           "Please reinstall Battleborn Reborn."};
        return {false, "The update could not be installed because a game file is in use. "
                       "Nothing was changed. Please try again after restarting the game."};
    } catch (...) {
        return {false, kNothingChanged};
    }
}

void CleanupOld(const fs::path& win64) {
    try {
        for (const char* name : {"reborn.dll.old", "dxgi.dll.old", "reborn.dll.bad"}) RemoveQuietly(win64 / name);
    } catch (...) {
    }
}

}  // namespace PlayerUpdate

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Player updates: the pure part (no game SDK), so the unit tests can build it.
// A player update replaces only reborn.dll and dxgi.dll in the game's Win64 folder.
// The manifest is signed (ECDSA P-256 over SHA-256, r||s base64) with the owner's key;
// the public half is UpdateKey::PublicKey.
namespace PlayerUpdate {

struct FileEntry {
    std::string name;     // "reborn.dll" or "dxgi.dll", nothing else
    std::string sha256;   // 64 lowercase hex characters
    uint64_t size;        // 1 byte .. 64 MiB
};

struct Manifest {
    std::string version;   // "1.0.N"
    int build;             // N, at least 1
    std::string notes;     // shown to the player; may be empty
    std::vector<FileEntry> files;
};

// The largest file a player update may carry.
constexpr uint64_t kMaxFileSize = 64ull * 1024 * 1024;

// Parses manifest.json: {"version","build","notes","files":[{"name","sha256","size"}]}.
// Returns nothing for anything that is not exactly that shape (missing or wrong-typed fields,
// a name other than reborn.dll/dxgi.dll, a repeated name, size 0 or over 64 MiB, a sha256 that
// is not 64 lowercase hex characters, no files, build below 1, a version other than exactly
// "1.0.<build>", notes over 4096 bytes).
std::optional<Manifest> ParseManifest(std::string_view json);

// True only when sigB64 (r||s, 64 bytes, base64) is a valid ECDSA P-256 / SHA-256 signature of
// data under pubB64 (X||Y, 64 bytes, base64). Leading/trailing whitespace on the base64 is ignored.
bool VerifySignature(std::string_view data, std::string_view sigB64, std::string_view pubB64);

// Lowercase hex SHA-256 of bytes; empty string if Windows' crypto API fails.
std::string Sha256Hex(std::string_view bytes);

// Offer an update only to a released build (> 0), only when the manifest is newer, and never the
// build that would not start here before (refusedBuild; 0 = none). A newer build is offered again.
bool ShouldOffer(const Manifest& m, int currentBuild, int refusedBuild);

struct InstallOutcome {
    bool ok;
    // Plain words for the player when ok is false ("" when ok). Always fixed text in the program,
    // so making or returning an outcome never allocates (InstallFiles' error paths rely on that).
    std::string_view message;
};

// Files in Win64 that remember an update across starts, one build number each:
// pending = the build just installed, waiting for the next start to load it;
// refused = a build that would not start, so dxgi put the previous mod back. Never offered again.
constexpr const char* kPendingFile = "reborn-update-pending.txt";
constexpr const char* kRefusedFile = "reborn-update-refused.txt";

// "7" (spaces and line ends around it allowed) -> 7. Anything else -> 0.
int ParseBuildNumber(std::string_view text);

// The build number in the file, or 0 if it is missing or not a build number. Never throws.
int ReadBuildFile(const std::filesystem::path& file);

// Writes the build number (and a line end) to the file. False if that did not work. Never throws.
bool WriteBuildFile(const std::filesystem::path& file, int build);

// At a player's start, before CleanupOld: if dxgi put the previous mod back (reborn.dll.bad is there),
// the build that was waiting to load (the pending file) becomes the refused one. The pending file is
// removed either way. Returns the refused build (0 = none). Never throws.
int NoteRefusedBuildAtStart(const std::filesystem::path& win64);

// Installs the manifest's files into win64 from bytesByName. Every file's size and hash is
// checked before anything is written. Then each file is written as <name>.new, and for each
// file <name> -> <name>.old and <name>.new -> <name> (the second rename never replaces a file).
// Any failure, including not being able to tell whether <name> exists, puts back what was already
// swapped and deletes the .new files, so the old version stays installed. Never throws.
InstallOutcome InstallFiles(const std::filesystem::path& win64, const Manifest& m,
                            const std::map<std::string, std::string>& bytesByName);

// Deletes reborn.dll.old, dxgi.dll.old, reborn.dll.bad, reborn.dll.new and dxgi.dll.new if
// present. Never throws.
void CleanupOld(const std::filesystem::path& win64);

}  // namespace PlayerUpdate

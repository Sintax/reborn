#pragma once

#include <algorithm>
#include <string>
#include <vector>
#include "json.hpp"

// Gear sets ("loadouts"): a player's saved sets of three gear items, shown in the loadout row of
// the game's own hero screen. The part with no game objects in it, so the unit tests can build it.
//
// The client keeps its sets in its .rebornsave and sends them to the server when it joins (on the
// ServerProcessConvolve channel, like the old single-gear message). The server puts them in the
// player's meta PRI, where the hero screen reads them (see GearSetsGame.cpp).
namespace GearSets {
    // Sets a player can keep. The game has room for 75 (ReplicatedLoadouts[0x4B]); the row shows
    // a handful, and a short list is easier to pick from.
    constexpr int kMaxSets = 10;
    constexpr size_t kMaxNameLength = 24;
    // Longest object name accepted from a client ("PoplarPerkFunction GD_....Perk_...").
    constexpr size_t kMaxPerkNameLength = 256;

    struct Set {
        int index = 0;               // the set's place in the client's save
        std::string name;
        std::string perks[3];        // full object names; "" is an empty slot
    };

    inline bool HasGear(const Set& s) {
        return !s.perks[0].empty() || !s.perks[1].empty() || !s.perks[2].empty();
    }

    inline std::string DefaultName(int index) { return "Set " + std::to_string(index + 1); }

    // Names are shown in the game's row: printable ASCII only, trimmed, never empty, short.
    inline std::string CleanName(const std::string& raw, int index) {
        std::string out;
        for (char c : raw) {
            if (c >= 0x20 && c < 0x7f) out += c;
            if (out.size() >= kMaxNameLength) break;
        }
        size_t a = out.find_first_not_of(' ');
        if (a == std::string::npos) return DefaultName(index);
        size_t b = out.find_last_not_of(' ');
        return out.substr(a, b - a + 1);
    }

    // Perk names travel without their class ("PoplarPerkFunction "), to keep messages small.
    inline const std::string& PerkClassPrefix() { static const std::string p = "PoplarPerkFunction "; return p; }

    inline std::string ShortPerkName(const std::string& full) {
        return full.starts_with(PerkClassPrefix()) ? full.substr(PerkClassPrefix().size()) : full;
    }

    inline std::string FullPerkName(const std::string& shortName) {
        if (shortName.empty() || shortName.find(' ') != std::string::npos) return shortName;
        return PerkClassPrefix() + shortName;
    }

    // The client's messages, one per set: {"NEMA":true,"part":k,"of":n,"gearSets":[{"index":i,
    // "name":"...","perks":["","",""]}]}. One set per message: a message has to fit one network
    // packet (UE3 drops a bunch over 512 bytes; three sets in one message never arrived). With no
    // sets, one message with "of":0 and an empty list, so the server stops waiting at once.
    // Only sets with gear in them are sent (an empty set would be an invalid loadout to the game);
    // each keeps its save index so the pick maps back to the right set.
    inline std::vector<std::string> ToMessages(const std::vector<Set>& sets) {
        std::vector<nlohmann::json> entries;
        for (const Set& s : sets) {
            if (!HasGear(s)) continue;
            if ((int)entries.size() >= kMaxSets) break;
            nlohmann::json e;
            e["index"] = s.index;
            e["name"] = CleanName(s.name, s.index);
            e["perks"] = { ShortPerkName(s.perks[0]), ShortPerkName(s.perks[1]), ShortPerkName(s.perks[2]) };
            entries.push_back(e);
        }
        std::vector<std::string> out;
        int n = (int)entries.size();
        for (int k = 0; k < (std::max)(n, 1); k++) {
            nlohmann::json j;
            j["NEMA"] = true;
            j["part"] = k;
            j["of"] = n;
            j["gearSets"] = nlohmann::json::array();
            if (k < n) j["gearSets"].push_back(entries[k]);
            out.push_back(j.dump());
        }
        return out;
    }

    // How many messages the sender said it sends (0..kMaxSets), or -1 if the message does not say.
    inline int PartsExpected(const nlohmann::json& j) {
        if (!j.is_object() || !j.contains("of") || !j["of"].is_number_integer()) return -1;
        int n = j["of"].get<int>();
        return n >= 0 && n <= kMaxSets ? n : -1;
    }

    inline bool IsGearSetsMessage(const nlohmann::json& j) {
        return j.is_object() && j.contains("gearSets");
    }

    // The server's reading of that message. Anything malformed is skipped, never trusted: at most
    // kMaxSets sets, indexes 0..kMaxSets-1 and each at most once, names cleaned, over-long perk
    // names dropped. A set left with no gear is skipped.
    inline std::vector<Set> FromMessage(const nlohmann::json& j) {
        std::vector<Set> out;
        if (!IsGearSetsMessage(j) || !j["gearSets"].is_array()) return out;
        bool seen[kMaxSets] = {};
        for (const auto& e : j["gearSets"]) {
            if ((int)out.size() >= kMaxSets) break;
            if (!e.is_object() || !e.contains("index") || !e["index"].is_number_integer()) continue;
            int index = e["index"].get<int>();
            if (index < 0 || index >= kMaxSets || seen[index]) continue;
            Set s;
            s.index = index;
            s.name = CleanName(e.contains("name") && e["name"].is_string() ? e["name"].get<std::string>() : "", index);
            if (e.contains("perks") && e["perks"].is_array()) {
                for (size_t k = 0; k < 3 && k < e["perks"].size(); k++) {
                    const auto& p = e["perks"][k];
                    if (p.is_string() && p.get<std::string>().size() <= kMaxPerkNameLength) s.perks[k] = FullPerkName(p.get<std::string>());
                }
            }
            if (!HasGear(s)) continue;
            seen[index] = true;
            out.push_back(s);
        }
        return out;
    }

    // The hero screen's "loadouts received" check (battleborn+0x112f580) counts a loadout as valid
    // when at least one of its three perks has rarity 1..5, a perk object, and an item level >= 0.
    // The count over all 75 entries must equal NumLoadoutEntitlements, or that player's hero screen
    // waits forever. The server only fills perks that pass this, and only counts sets that do.
    struct PerkFields { int rarity = 0; bool hasPerk = false; int itemLevel = 0; };

    inline bool PerkCounts(const PerkFields& p) {
        return (unsigned char)(p.rarity - 1) < 5 && p.hasPerk && p.itemLevel >= 0;
    }

    inline bool LoadoutCounts(const PerkFields (&perks)[3]) {
        return PerkCounts(perks[0]) || PerkCounts(perks[1]) || PerkCounts(perks[2]);
    }
}

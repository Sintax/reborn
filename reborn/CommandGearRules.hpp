#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "json.hpp"

// The main menu's Command screen -> Gear tab, offline: the replies the game would have had from
// its online item service (Hydra), built from the local save. The part with no game objects in
// it, so the unit tests can build it. Game side: CommandGear.cpp.
namespace CommandGearRules {
    // One gear item as the Gear tab gets it.
    struct GearItem {
        std::string guid;          // stable local id (GuidFor); also the item's "item_id"
        std::string name;          // shown name
        std::string description;
        std::string assetKey;      // the perk's AssetTrackerKey: the game finds the perk object by it
        int rarity = 0;            // 1 Common .. 5 Legendary
        int level = 0;
    };

    // One saved gear set as the Loadouts list gets it. Ids start at 1 (the page numbers its
    // loadout slots 1..n).
    struct GearLoadout {
        int id = 1;
        std::string name;
        std::string perks[3];      // item guids; "" is an empty slot
    };

    // The Command rank the screen is told (Battleborn's top rank).
    constexpr int kCommandRank = 100;

    // A save's loadout index <-> the game's loadout id.
    inline int LoadoutIdFor(int saveIndex) { return saveIndex + 1; }
    inline int SaveIndexFor(int loadoutId) { return loadoutId - 1; }

    // Same object name, same guid, every launch (FNV-1a 64). Saves only ever hold object names, so
    // the guid is what links the game's loadout edits back to them.
    inline std::string GuidFor(const std::string& objectName) {
        uint64_t h = 0xcbf29ce484222325ull;
        for (unsigned char c : objectName) { h ^= c; h *= 0x100000001b3ull; }
        char buf[24];
        std::snprintf(buf, sizeof(buf), "rb%016llx", (unsigned long long)h);
        return buf;
    }

    // The rarity words the game's json_data parser knows (battleborn+0x116db70).
    inline const char* RarityWord(int rarity) {
        switch (rarity) {
        case 1: return "Common";
        case 2: return "Uncommon";
        case 3: return "Rare";
        case 4: return "VeryRare";
        case 5: return "Legendary";
        default: return "Common";
        }
    }

    // Rarity from a gear object name ("...PF_Gear_ShieldPen_Epic_ELD_Flawed" -> 4). Epic is the
    // game's Very Rare.
    inline int RarityFromObjectName(const std::string& name) {
        if (name.find("Legendary") != std::string::npos) return 5;
        if (name.find("VeryRare") != std::string::npos || name.find("Epic") != std::string::npos) return 4;
        if (name.find("Uncommon") != std::string::npos) return 2;
        if (name.find("Rare") != std::string::npos) return 3;
        return 1;
    }

    // Standard base64 with padding: the game base64-decodes json_data before reading it
    // (battleborn+0x116d380 -> +0x62310).
    inline std::string Base64(const std::string& in) {
        static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        size_t i = 0;
        for (; i + 2 < in.size(); i += 3) {
            uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8) | (uint8_t)in[i + 2];
            out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += t[v & 63];
        }
        if (i + 1 == in.size()) {
            uint32_t v = (uint8_t)in[i] << 16;
            out += t[v >> 18]; out += t[(v >> 12) & 63]; out += "==";
        }
        else if (i + 2 == in.size()) {
            uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8);
            out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += '=';
        }
        return out;
    }

    // json_data of one item, base64: the perk (by asset key) and its rarity (battleborn+0x16f1f20).
    // The text is encoded with its closing zero byte: the game copies the decoded bytes without
    // one and then reads them up to a zero (battleborn+0x116d380), so without it the json gets
    // whatever follows in memory and does not parse.
    inline std::string ItemJsonData(const GearItem& item) {
        nlohmann::json j;
        j["perk_function_0"] = item.assetKey;
        j["rarity"] = RarityWord(item.rarity);
        return Base64(j.dump() + std::string(1, '\0'));
    }

    inline nlohmann::json UserItem(const GearItem& item) {
        nlohmann::json u;
        u["item_id"] = item.guid;
        u["guid"] = item.guid;
        u["name"] = item.name;
        u["description"] = item.description;
        u["can_use"] = true;
        u["category_name"] = "Perk";      // battleborn+0x1166d20: "Perk" -> type 8, a gear item
        u["item_level"] = item.level;
        u["json_data"] = ItemJsonData(item);
        return u;
    }

    // Reply to GET /v1/poplar/client/sideboards (battleborn+0x15a2d00): every gear item, and a bank
    // big enough to show them all (the bank list is cut to bank_size).
    inline std::string SideboardsReply(const std::vector<GearItem>& items) {
        nlohmann::json j;
        j["meta_profile"]["user_items"] = nlohmann::json::array();
        for (const GearItem& item : items) j["meta_profile"]["user_items"].push_back(UserItem(item));
        j["meta_profile"]["meta_game"]["earned_currency"] = 0;
        j["meta_profile"]["meta_game"]["bank_size"] = (int)items.size();
        return j.dump();
    }

    // Reply to GET /v1/poplar/client/meta_profiles (battleborn+0x15a3860 -> +0x15a3660): the saved
    // sets, and how many loadout slots the player has (entitled_loadouts, at least the sets).
    inline std::string MetaProfileReply(const std::vector<GearLoadout>& loadouts, int slots, int bankSize) {
        nlohmann::json j;
        nlohmann::json list = nlohmann::json::array();
        for (const GearLoadout& l : loadouts) {
            nlohmann::json e;
            e["loadout_id"] = l.id;
            e["version"] = 1;
            e["name"] = l.name;
            e["loadout_structure"]["perks"] = nlohmann::json::array();
            for (int k = 0; k < 3; k++) {
                if (l.perks[k].empty()) continue;
                nlohmann::json p;
                p["slot_id"] = k;
                p["item_id"] = l.perks[k];
                p["item_level"] = 0;
                p["can_use"] = true;
                e["loadout_structure"]["perks"].push_back(p);
            }
            list.push_back(e);
        }
        j["meta_profile"]["loadouts"]["entitled_loadouts"] = (std::max)(slots, (int)loadouts.size());
        j["meta_profile"]["loadouts"]["loadouts"] = list;
        j["meta_profile"]["meta_game"]["earned_currency"] = 0;
        j["meta_profile"]["meta_game"]["bank_size"] = bankSize;
        // Command rank (battleborn+0x116c0a0 -> profile +0xa0). The gear bank is locked below the
        // menu's MinCommandRankForGear (3), so the player is shown at the top rank.
        j["meta_profile"]["meta_game"]["meta_level"] = kCommandRank;
        j["meta_profile"]["meta_game"]["max_meta_level"] = kCommandRank;
        return j.dump();
    }

    // The body of the game's save request, POST /v2/poplar/client/meta_profiles/loadouts
    // (battleborn+0x116e790): {"loadout":{"version":v,"loadout_id":i,"name":"..","structure":
    // {"perks":["guid","guid","guid"]}}}. The game writes the name unescaped, so a name with a
    // quote in it does not parse; ok is false then.
    struct SavedLoadout {
        bool ok = false;
        int id = 0;
        int version = 0;
        std::string name;
        std::string perks[3];
    };

    inline SavedLoadout ParseSaveRequest(const std::string& body) {
        SavedLoadout out;
        nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
        if (j.is_discarded() || !j.is_object() || !j.contains("loadout") || !j["loadout"].is_object()) return out;
        const nlohmann::json& l = j["loadout"];
        if (!l.contains("loadout_id") || !l["loadout_id"].is_number_integer()) return out;
        out.id = l["loadout_id"].get<int>();
        if (l.contains("version") && l["version"].is_number_integer()) out.version = l["version"].get<int>();
        if (l.contains("name") && l["name"].is_string()) out.name = l["name"].get<std::string>();
        if (l.contains("structure") && l["structure"].is_object() && l["structure"].contains("perks") && l["structure"]["perks"].is_array()) {
            const nlohmann::json& p = l["structure"]["perks"];
            for (size_t k = 0; k < 3 && k < p.size(); k++)
                if (p[k].is_string()) out.perks[k] = p[k].get<std::string>();
        }
        out.ok = out.id >= 1;
        return out;
    }
}

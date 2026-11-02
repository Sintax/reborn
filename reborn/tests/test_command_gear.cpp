// reborn/tests/test_command_gear.cpp
#include "../CommandGearRules.hpp"
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

using namespace CommandGearRules;

int main() {
    // Guids: stable, distinct, short
    CHECK(GuidFor("PoplarPerkFunction A") == GuidFor("PoplarPerkFunction A"));
    CHECK(GuidFor("PoplarPerkFunction A") != GuidFor("PoplarPerkFunction B"));
    CHECK(GuidFor("x").size() == 18 && GuidFor("x").rfind("rb", 0) == 0);

    // Loadout ids start at 1
    CHECK(LoadoutIdFor(0) == 1 && SaveIndexFor(1) == 0);

    // Rarity words the game knows
    CHECK(std::string(RarityWord(1)) == "Common");
    CHECK(std::string(RarityWord(4)) == "VeryRare");
    CHECK(std::string(RarityWord(5)) == "Legendary");
    CHECK(std::string(RarityWord(0)) == "Common");

    CHECK(RarityFromObjectName("GD_Gear.Gear.Epic.PF_Gear_ShieldPen_Epic_ELD_Flawed") == 4);
    CHECK(RarityFromObjectName("GD_Gear.Gear.Uncommon.PF_Gear_X_Uncommon") == 2);
    CHECK(RarityFromObjectName("PF_Gear_X_Rare_JNT") == 3);
    CHECK(RarityFromObjectName("PF_Gear_SkillDamage_Legendary_JNT2") == 5);
    CHECK(RarityFromObjectName("PF_Gear_X_Common") == 1);

    // Sideboards reply
    GearItem a; a.guid = "rb1"; a.name = "Rare Thing"; a.description = "d"; a.assetKey = "123"; a.rarity = 3; a.level = 2;
    nlohmann::json s = nlohmann::json::parse(SideboardsReply({ a }));
    CHECK(s["meta_profile"]["user_items"].size() == 1);
    const nlohmann::json& u = s["meta_profile"]["user_items"][0];
    CHECK(u["item_id"] == "rb1" && u["guid"] == "rb1");
    CHECK(u["category_name"] == "Perk");
    CHECK(u["can_use"] == true);
    CHECK(u["item_level"] == 2);
    CHECK(Base64("") == "" && Base64("f") == "Zg==" && Base64("fo") == "Zm8=" && Base64("foo") == "Zm9v" && Base64("foob") == "Zm9vYg==");
    CHECK(u["json_data"] == Base64(std::string("{\"perk_function_0\":\"123\",\"rarity\":\"Rare\"}") + '\0'));
    nlohmann::json data = nlohmann::json::parse("{\"perk_function_0\":\"123\",\"rarity\":\"Rare\"}");
    CHECK(data["perk_function_0"] == "123" && data["rarity"] == "Rare");
    CHECK(s["meta_profile"]["meta_game"]["bank_size"] == 1);

    // Meta profile reply: empty slots left out, at least as many slots as sets
    GearLoadout l; l.id = 1; l.name = "Tank"; l.perks[0] = "rb1"; l.perks[2] = "rb2";
    nlohmann::json m = nlohmann::json::parse(MetaProfileReply({ l }, 10, 5));
    CHECK(m["meta_profile"]["loadouts"]["entitled_loadouts"] == 10);
    CHECK(m["meta_profile"]["meta_game"]["meta_level"] == kCommandRank);
    CHECK(m["meta_profile"]["loadouts"]["loadouts"].size() == 1);
    const nlohmann::json& perks = m["meta_profile"]["loadouts"]["loadouts"][0]["loadout_structure"]["perks"];
    CHECK(perks.size() == 2);
    CHECK(perks[0]["slot_id"] == 0 && perks[0]["item_id"] == "rb1");
    CHECK(perks[1]["slot_id"] == 2 && perks[1]["item_id"] == "rb2");
    CHECK(nlohmann::json::parse(MetaProfileReply({ l, l, l }, 2, 0))["meta_profile"]["loadouts"]["entitled_loadouts"] == 3);

    // Save request, as the game writes it
    SavedLoadout sv = ParseSaveRequest("{\"loadout\":{\"version\":1,\"loadout_id\":2,\"name\":\"Damage\",\"structure\":{\"perks\":[\"rb1\",\"\",\"rb3\"]}}}");
    CHECK(sv.ok && sv.id == 2 && sv.version == 1 && sv.name == "Damage");
    CHECK(sv.perks[0] == "rb1" && sv.perks[1].empty() && sv.perks[2] == "rb3");
    CHECK(!ParseSaveRequest("{\"loadout\":{\"loadout_id\":1,\"name\":\"a\"b\"}}").ok);   // unescaped quote
    CHECK(!ParseSaveRequest("{\"loadout\":{\"loadout_id\":0}}").ok);
    CHECK(!ParseSaveRequest("").ok);

    // The reply to a save gives the saved set back exactly as sent (else the page saves forever)
    SavedLoadout unnamed = ParseSaveRequest("{\"loadout\":{\"version\":1,\"loadout_id\":1,\"name\":\"\",\"structure\":{\"perks\":[\"3ACD9B46\",\"rb2\",\"\"]}}}");
    GearLoadout s1; s1.id = 1; s1.name = "Set 1"; s1.perks[0] = "rb9"; s1.perks[1] = "rb2"; s1.perks[2] = "rb3";
    GearLoadout s2; s2.id = 2; s2.name = "Set 2"; s2.perks[0] = "rb4";
    std::vector<GearLoadout> sets = { s1, s2 };
    EchoSaved(sets, unnamed);
    CHECK(sets[0].name.empty() && sets[0].perks[0] == "3ACD9B46" && sets[0].perks[1] == "rb2" && sets[0].perks[2].empty());
    CHECK(sets[1].name == "Set 2" && sets[1].perks[0] == "rb4");

    if (failures == 0) std::printf("PASS\n");
    return failures == 0 ? 0 : 1;
}

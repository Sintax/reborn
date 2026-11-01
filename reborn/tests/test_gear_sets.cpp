// reborn/tests/test_gear_sets.cpp
#include "../GearSets.hpp"
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

using namespace GearSets;

static Set MakeSet(int index, const std::string& name, const std::string& a, const std::string& b = "", const std::string& c = "") {
    Set s; s.index = index; s.name = name; s.perks[0] = a; s.perks[1] = b; s.perks[2] = c;
    return s;
}

int main() {
    // Names
    CHECK(CleanName("  Tank  ", 0) == "Tank");
    CHECK(CleanName("", 2) == "Set 3");
    CHECK(CleanName("   ", 0) == "Set 1");
    CHECK(CleanName("a\x01" "b\n", 0) == "ab");
    CHECK(CleanName(std::string(40, 'x'), 0).size() == kMaxNameLength);

    // Round trip: empty sets are not sent, indexes kept
    std::vector<Set> sets = { MakeSet(0, "Tank", "PoplarPerkFunction A", "", "PoplarPerkFunction C"),
                              MakeSet(1, "Empty", ""),
                              MakeSet(2, "", "PoplarPerkFunction B") };
    std::vector<std::string> msgs = ToMessages(sets);
    CHECK(msgs.size() == 2);
    std::vector<Set> got;
    for (size_t k = 0; k < msgs.size(); k++) {
        nlohmann::json j = nlohmann::json::parse(msgs[k]);
        CHECK(j["NEMA"] == true);
        CHECK(IsGearSetsMessage(j));
        CHECK(PartsExpected(j) == 2);
        CHECK(j["part"] == (int)k);
        CHECK(msgs[k].find("PoplarPerkFunction") == std::string::npos);   // class prefix left off on the wire
        for (const Set& s : FromMessage(j)) got.push_back(s);
    }
    CHECK(got.size() == 2);
    CHECK(got[0].index == 0 && got[0].name == "Tank" && got[0].perks[0] == "PoplarPerkFunction A" && got[0].perks[1].empty());
    CHECK(got[1].index == 2 && got[1].name == "Set 3" && got[1].perks[0] == "PoplarPerkFunction B");

    // A real-length set fits one packet (UE3 drops a bunch over 512 bytes)
    Set big = MakeSet(9, std::string(30, 'N'), "PoplarPerkFunction GD_Gear.Gear.VeryRare.PF_Gear_ShieldRegen_VeryRare_JNT_Flawed",
        "PoplarPerkFunction GD_Gear_DAH.Gear.PF_Gear_SkillDamage_Legendary_JNT2_Flawed",
        "PoplarPerkFunction GD_Gear.Gear.Uncommon.PF_Gear_RecoilRedux_Uncommon_ELD_Flawed");
    CHECK(ToMessages({ big })[0].size() < 400);

    // No sets: one message saying so
    msgs = ToMessages({ MakeSet(0, "Empty", "") });
    CHECK(msgs.size() == 1);
    CHECK(PartsExpected(nlohmann::json::parse(msgs[0])) == 0);
    CHECK(FromMessage(nlohmann::json::parse(msgs[0])).empty());
    CHECK(PartsExpected(nlohmann::json::parse(R"({"of":11})")) == -1);
    CHECK(PartsExpected(nlohmann::json::parse(R"({"of":"2"})")) == -1);

    // Names
    CHECK(ShortPerkName("PoplarPerkFunction GD_X.Y") == "GD_X.Y");
    CHECK(FullPerkName("GD_X.Y") == "PoplarPerkFunction GD_X.Y");
    CHECK(FullPerkName("").empty());

    // At most kMaxSets are sent
    std::vector<Set> many;
    for (int i = 0; i < 15; i++) many.push_back(MakeSet(i, "", "P"));
    CHECK(ToMessages(many).size() == (size_t)kMaxSets);

    // The old single-gear message is not a gear-sets message
    CHECK(!IsGearSetsMessage(nlohmann::json::parse(R"({"NEMA":true,"perkOne":"x"})")));

    // Hostile input: bad types, out-of-range and repeated indexes, long names, too many sets
    nlohmann::json bad = nlohmann::json::parse(R"({"NEMA":true,"gearSets":[
        5, {"index":"0","perks":["A"]}, {"index":-1,"perks":["A"]}, {"index":10,"perks":["A"]},
        {"index":3,"perks":["A"]}, {"index":3,"perks":["B"]}, {"index":4,"perks":[1,2,3]},
        {"index":6,"name":7,"perks":["A","B","C","D"]}]})");
    bad["gearSets"].push_back({ {"index", 7}, {"perks", { std::string(300, 'x') }} });
    got = FromMessage(bad);
    CHECK(got.size() == 2);
    CHECK(got[0].index == 3 && got[0].perks[0] == "PoplarPerkFunction A");
    CHECK(got[1].index == 6 && got[1].name == "Set 7" && got[1].perks[2] == "PoplarPerkFunction C");
    CHECK(FromMessage(nlohmann::json::parse(R"({"gearSets":{}})")).empty());
    nlohmann::json flood; flood["gearSets"] = nlohmann::json::array();
    for (int i = 0; i < 50; i++) flood["gearSets"].push_back({ {"index", i % 10}, {"perks", {"A"}} });
    CHECK(FromMessage(flood).size() == (size_t)kMaxSets);

    // The hero screen's validity rule (battleborn+0x112f580)
    CHECK(PerkCounts({ 1, true, 0 }));
    CHECK(PerkCounts({ 5, true, 0 }));
    CHECK(!PerkCounts({ 0, true, 0 }));
    CHECK(!PerkCounts({ 6, true, 0 }));
    CHECK(!PerkCounts({ 3, false, 0 }));
    CHECK(!PerkCounts({ 3, true, -1 }));
    PerkFields one[3] = { { 0, false, 0 }, { 4, true, 0 }, { 0, false, 0 } };
    PerkFields none[3] = {};
    CHECK(LoadoutCounts(one));
    CHECK(!LoadoutCounts(none));

    if (failures == 0) std::printf("PASS\n");
    return failures == 0 ? 0 : 1;
}

#include "GearDump.hpp"
#include "Diagnostics.hpp"
#include "LaunchOptions.hpp"
#include "Utils.hpp"
#include "json.hpp"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>

namespace GearDump {
    namespace {
        bool g_done = false;
        float g_since = 0.0f;

        using json = nlohmann::json;

        std::string Name(UObject* o) { return o ? o->GetFullName() : std::string(); }

        float Eval(const FAttributeInitializationData& d, UObject* context) {
            __try { return UAttributeInitializationDefinition::EvaluateInitializationData(d, context, nullptr); }
            __except (1) { return -999999.0f; }
        }

        struct Dumper {
            json defs = json::object();
            std::unordered_set<UAttributeInitializationDefinition*> seen;
            UPoplarPerkFunction* perk = nullptr;   // the piece being dumped: its item level feeds the formulas

            // The formula evaluated with the piece at this item level (ItemLevelOverride units).
            float EvalAtLevel(const FAttributeInitializationData& d, UObject* context, int level) {
                if (!perk) return Eval(d, context);
                uint32_t useBefore = perk->bUseItemLevelOverride;
                int levelBefore = perk->ItemLevelOverride;
                perk->bUseItemLevelOverride = true;
                perk->ItemLevelOverride = level;
                float v = Eval(d, context);
                perk->bUseItemLevelOverride = useBefore;
                perk->ItemLevelOverride = levelBefore;
                return v;
            }

            // One AttributeInitializationData: its raw fields, plus what the game evaluates it to
            // as shipped ("v") and at item levels 0, 25000 and 50000 ("v0", "v25", "v50").
            json Data(const FAttributeInitializationData& d, UObject* context) {
                json j = { {"c", d.BaseValueConstant}, {"s", d.BaseValueScaleConstant}, {"v", Eval(d, context)},
                    {"v0", EvalAtLevel(d, context, 0)}, {"v25", EvalAtLevel(d, context, 25000)}, {"v50", EvalAtLevel(d, context, 50000)} };
                if (d.BaseValueAttribute) j["a"] = Name(d.BaseValueAttribute);
                if (d.InitializationDefinition) {
                    j["d"] = Name(d.InitializationDefinition);
                    Def(d.InitializationDefinition, context);
                }
                return j;
            }

            void Def(UAttributeInitializationDefinition* def, UObject* context) {
                if (!def || !seen.insert(def).second) return;
                json j = { {"mode", (int)def->BaseValueMode}, {"round", (int)def->RoundingMode} };
                const FBalanceFormula& f = def->ValueFormula;
                if (f.bEnabled) j["formula"] = { {"mul", Data(f.Multiplier, context)}, {"lvl", Data(f.Level, context)},
                    {"pow", Data(f.Power, context)}, {"off", Data(f.Offset, context)} };
                const FConditionalInitializationExpressions& c = def->ConditionalInitialization;
                if (c.bEnabled) {
                    json list = json::array();
                    for (int i = 0; i < c.ConditionalExpressionList.ArrayCount; i++) {
                        const FConditionalValueExpression& e = c.ConditionalExpressionList[i];
                        list.push_back({ {"ifTrue", Data(e.BaseValueIfTrue, context)}, {"cond", Name(e.Condition)},
                            {"exprs", e.Expressions.ArrayCount} });
                    }
                    j["conditional"] = { {"list", list}, {"default", Data(c.DefaultBaseValue, context)} };
                }
                const FCoefficientFormula& k = def->AddedCoefficientFormula;
                if (k.bEnabled) j["coef"] = { {"m1", Data(k.Modifier1, context)}, {"c1", Data(k.Coefficient1, context)},
                    {"m2", Data(k.Modifier2, context)}, {"c2", Data(k.Coefficient2, context)} };
                const FVariance& v = def->RandomVariance;
                if (v.bEnabled) j["variance"] = { {"lo", Data(v.LowerBound, context)}, {"hi", Data(v.UpperBound, context)}, {"int", (bool)v.bUseIntegerRandomization} };
                const FRange& r = def->RangeRestriction;
                if (r.bEnableMinValueRestriction) j["min"] = Data(r.MinValue, context);
                if (r.bEnableMaxValueRestriction) j["max"] = Data(r.MaxValue, context);
                defs[Name(def)] = j;
            }

            json Presentation(UAttributePresentationDefinition* p, UObject* context) {
                json j = { {"obj", Name(p)}, {"attr", Name(p->Attribute)}, {"desc", p->Description.ToString()},
                    {"prefix", p->Prefix.ToString()}, {"suffix", p->Suffix.ToString()}, {"pct", (bool)p->bDisplayAsPercentage},
                    {"pctFloat", (bool)p->bDisplayPercentAsFloat}, {"inverse", (bool)p->bDisplayAsInverse}, {"noNumber", (bool)p->bDontDisplayNumber},
                    {"precision", p->FloatPrecision} };
                if (p->IsA(UMarkupAttributePresentationDefinition::StaticClass())) {
                    auto* m = reinterpret_cast<UMarkupAttributePresentationDefinition*>(p);
                    j["replace"] = m->StringToReplace.ToString();
                    j["formula"] = Data(m->AttributeInitFormula, context);
                }
                return j;
            }

            // Conditional gear bonuses and hero effects carry no effect of their own: their event
            // responses and activation actions start another skill, so those are followed too.
            json Skill(USkillDefinition* s, UObject* context, int depth = 0) {
                json j = { {"obj", Name(s)}, {"name", s->SkillName.ToString()}, {"desc", s->SkillDescription.ToString()},
                    {"locked", s->LockedSkillDetailStr.ToString()}, {"type", (int)s->SkillType}, {"durationType", (int)s->DurationType},
                    {"duration", Data(s->InitialDuration, context)}, {"constraints", s->SkillConstraints.ArrayCount},
                    {"damageEvents", s->DamageEvents.ArrayCount}, {"killEvents", s->KillEvents.ArrayCount} };
                json events = json::array();
                for (int i = 0; i < s->EventResponses.ArrayCount; i++) {
                    const FSkillEventResponseData& e = s->EventResponses[i];
                    json ev = { {"event", (int)e.EventType} };
                    if (e.Action.SkillToActivate) ev["activate"] = depth < 3 ? Skill(e.Action.SkillToActivate, context, depth + 1) : json(Name(e.Action.SkillToActivate));
                    if (e.Action.SkillToDeactivate) ev["deactivate"] = Name(e.Action.SkillToDeactivate);
                    events.push_back(ev);
                }
                for (int i = 0; i < s->SkillActivationActions.ArrayCount; i++) {
                    const FSkillActionData& a = s->SkillActivationActions[i];
                    json ev = { {"event", "activation"} };
                    if (a.SkillToActivate) ev["activate"] = depth < 3 ? Skill(a.SkillToActivate, context, depth + 1) : json(Name(a.SkillToActivate));
                    if (a.SkillToDeactivate) ev["deactivate"] = Name(a.SkillToDeactivate);
                    events.push_back(ev);
                }
                for (int i = 0; i < s->CustomEvents.ArrayCount; i++)
                    events.push_back({ {"event", "custom"}, {"name", s->CustomEvents[i].EventName.ToString()}, {"responses", s->CustomEvents[i].EventResponses.ArrayCount} });
                if (!events.empty()) j["events"] = events;
                json effects = json::array();
                for (int g = 0; g < s->SkillEffectGroups.ArrayCount; g++) {
                    const FSkillEffectGroupData& group = s->SkillEffectGroups[g];
                    for (int a = 0; a < group.Attributes.ArrayCount; a++) {
                        const FSkillEffectAttributeData& e = group.Attributes[a];
                        effects.push_back({ {"group", g}, {"attr", Name(e.AttributeToModify)}, {"type", (int)e.ModifierType},
                            {"value", Data(e.BaseModifierValue, context)}, {"target", (int)group.TargetCriteria.EffectTarget} });
                    }
                }
                for (int i = 0; i < s->SkillEffectDefinitions.ArrayCount; i++) {
                    const FSkillEffectData& e = s->SkillEffectDefinitions[i];
                    effects.push_back({ {"old", true}, {"attr", Name(e.AttributeToModify)}, {"type", (int)e.ModifierType},
                        {"value", Data(e.BaseModifierValue, context)}, {"target", (int)e.EffectTarget} });
                }
                j["effects"] = effects;
                json pres = json::array();
                for (int i = 0; i < s->SkillEffectPresentations.ArrayCount; i++)
                    if (s->SkillEffectPresentations[i]) pres.push_back(Presentation(s->SkillEffectPresentations[i], context));
                if (!pres.empty()) j["presentations"] = pres;
                return j;
            }

            json Perk(UPoplarPerkFunction* p) {
                perk = p;
                json j = { {"obj", Name(p)}, {"name", p->DisplayName.ToString()}, {"prefix", p->DisplayNamePrefix.ToString()},
                    {"short", p->ShortDescription.ToString()}, {"readable", p->ReadableNameString.ToString()},
                    {"rarity", p->RarityString.ToString()}, {"faction", Name(p->AssociatedFaction)},
                    {"passive", (bool)p->IsPassive}, {"campaignOnly", (bool)p->bCampaignOnly},
                    {"levelOverride", p->bUseItemLevelOverride ? p->ItemLevelOverride : -1},
                    {"cost", Data(p->BaseActivationCost, p)} };
                json pres = json::array();
                for (int i = 0; i < p->DescriptionPresentations.ArrayCount; i++)
                    if (p->DescriptionPresentations[i]) pres.push_back(Presentation(p->DescriptionPresentations[i], p));
                j["presentations"] = pres;
                json skills = json::array();
                for (int i = 0; i < p->SkillsToRunDuringBuff.ArrayCount; i++)
                    if (p->SkillsToRunDuringBuff[i]) skills.push_back(Skill(p->SkillsToRunDuringBuff[i], p));
                j["skills"] = skills;
                if (p->LifestealFormula.BaseValueConstant || p->LifestealFormula.InitializationDefinition) j["lifesteal"] = Data(p->LifestealFormula, p);
                if (p->ShieldPenFormula.BaseValueConstant || p->ShieldPenFormula.InitializationDefinition) j["shieldPen"] = Data(p->ShieldPenFormula, p);
                if (p->bUseTieredActivation) {
                    json tiers = json::array();
                    for (int i = 0; i < p->TierActivations.ArrayCount; i++)
                        tiers.push_back({ {"cost", Data(p->TierActivations[i].ActivationCost, p)}, {"card", p->TierActivations[i].ItemCardDescription.ToString()} });
                    j["tiers"] = tiers;
                }
                return j;
            }
        };

        void Run() {
            Dumper d;
            json perks = json::array();
            for (UPoplarPerkFunction* p : SDKUtils::GetAllOfClass<UPoplarPerkFunction>()) {
                if (!p || p->GetFullName().find("Default__") != std::string::npos) continue;
                perks.push_back(d.Perk(p));
            }
            // Bonuses that work through damage events (attack damage, crit damage, skill damage, the
            // flaws, the legendary effects) reference no formula from their skill, so every gear
            // formula is dumped by name as well, evaluated with the first piece as the context.
            UPoplarPerkFunction* context = nullptr;
            for (UPoplarPerkFunction* p : SDKUtils::GetAllOfClass<UPoplarPerkFunction>())
                if (p && p->GetFullName().find("Default__") == std::string::npos) { context = p; break; }
            d.perk = context;
            size_t named = 0;
            for (UAttributeInitializationDefinition* def : SDKUtils::GetAllOfClass<UAttributeInitializationDefinition>()) {
                if (!def || def->GetFullName().find("GD_Gear") == std::string::npos) continue;
                d.Def(def, context);
                named++;
            }
            json attrs = json::object();
            for (UAttributeDefinition* a : SDKUtils::GetAllOfClass<UAttributeDefinition>()) {
                if (!a || a->GetFullName().find("GD_Gear.Attributes") == std::string::npos) continue;
                FAttributeInitializationData ref{ 0.0f, a, nullptr, 1.0f };
                attrs[Name(a)] = d.Data(ref, context);
            }
            json out = { {"perks", perks}, {"defs", d.defs}, {"attrs", attrs} };
            printf("[GEARDUMP] %zu gear formulas by name, %zu gear attributes\n", named, attrs.size());
            std::filesystem::path path = std::filesystem::path(Diagnostics::RunDir()) / L"gear-stats.json";
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            f << out.dump(1);
            printf("[GEARDUMP] wrote %zu perks and %zu formulas to %s (%s)\n", perks.size(), d.defs.size(),
                path.string().c_str(), f ? "ok" : "FAILED");
        }
    }

    void Tick(float dt) {
        if (g_done || !LaunchOptions::Get().dumpGear) return;
        g_since += dt;
        if (g_since < 2.0f) return;
        g_since = 0.0f;
        // The gear packages load with the front end; wait until most of the ~800 pieces exist.
        if (SDKUtils::GetAllOfClass<UPoplarPerkFunction>().size() < 700) return;
        g_done = true;
        Run();
    }
}

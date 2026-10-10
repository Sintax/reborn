#include "GameState.hpp"
#include "Autopilot.hpp"
#include "Combat.hpp"
#include "Constants.hpp"
#include "Diagnostics.hpp"
#include "Engine.hpp"
#include "Globals.hpp"
#include "Hooks.hpp"
#include "LaunchOptions.hpp"
#include "PawnUtils.hpp"
#include "Utils.hpp"
#include "json.hpp"
#include <Windows.h>
#include <Psapi.h>
#include <atomic>
#include <algorithm>
#include <map>
#pragma comment(lib, "psapi.lib")

namespace GameState {
    namespace {
        std::atomic<bool> g_listening{ false };

        std::string MapName() {
            UWorld* world = Globals::GetGWorld();
            if (!world) return "";
            std::string full = world->GetFullName();          // e.g. "World Dojo_P.TheWorld"
            size_t sp = full.find(' '), dot = full.find('.');
            if (sp == std::string::npos || dot == std::string::npos || dot <= sp) return full;
            return full.substr(sp + 1, dot - sp - 1);
        }

        using PawnUtils::IsDefault;

        nlohmann::json Loc(AActor* a) { return { a->Location.X, a->Location.Y, a->Location.Z }; }

        // Hex of all 16 RawId bytes. The server sets RawId[0] per LAN player (Hooks.cpp) and the
        // field replicates, so server and client render the same player the same way.
        std::string UniqueIdString(const FUniqueNetId& id) {
            if (!id.bHasValue) return "";
            static const char* hex = "0123456789abcdef";
            std::string out;
            for (uint8_t b : id.RawId) { out += hex[b >> 4]; out += hex[b & 0xF]; }
            return out;
        }

        // The game's own match state (EMatchState), from the replicated game info, so a client
        // knows the match is over as soon as it shows Victory/Defeat. -1 when there is none.
        int MatchState() {
            APoplarGameReplicationInfo* gri = nullptr;
            for (APoplarGameReplicationInfo* g : SDKUtils::GetAllOfClass<APoplarGameReplicationInfo>())
                if (g && !(g->ObjectFlags & 0x2000000000000000) && !IsDefault(g)) gri = g;
            return gri ? (int)gri->CurrentMatchState.State : -1;
        }

        // --- Is a player's body drawn? ---------------------------------------------------------
        //
        // s3 run 20261006-023201: player 2's pawn never got its skin, so it had no Mesh (see the
        // [FLOOR] notes in Hooks.cpp: the game strips Mesh and the cylinder from pawn archetypes and
        // the skin puts them back). The collision guard gave it a cylinder, so it walked and the run
        // passed, but nobody could see it, not even its own player. These checks report it.
        using PawnUtils::Gone;
        using PawnUtils::LivePawnInWorld;

        // What keeps the pawn's body from being drawn, or "" when nothing does. checkHidden also
        // counts a mesh component marked hidden; not used for the local player's own pawn, whose
        // third-person body the first-person view may hide on purpose.
        std::string MissingBody(APawn* p, bool checkHidden) {
            if (p->bHidden) return "pawn hidden";
            USkeletalMeshComponent* m = p->Mesh;
            if (!m) return "no mesh component";
            if (Gone(m)) return "mesh component being destroyed";
            if (!m->SkeletalMesh) return "mesh component has no skeletal mesh";
            if (!m->bAttached) return "mesh component not attached";
            if (checkHidden && m->HiddenGame) return "mesh component hidden";
            return "";
        }

        using PawnUtils::HeroOf;

        // A human player's pawn: it has a PlayerReplicationInfo that is not a bot's. On a client the
        // other players' pawns have no Controller (it is not replicated to other clients), so the
        // PRI's replicated bBot flag is what tells a bot from a person; where the controller is
        // known (the server, or the own pawn), an AI controller also means a bot.
        bool IsHumanPawn(APawn* p) {
            APlayerReplicationInfo* pri = p->PlayerReplicationInfo;
            if (Gone(pri) || pri->bBot) return false;
            return !p->Controller || p->Controller->IsA(APlayerController::StaticClass());
        }

        std::string NameOf(UObject* o) { return Gone(o) ? std::string("none") : o->GetFullName(); }

        // One [VISIBLE] line the first time a pawn is seen without its body, and one when it gets
        // it, so a pawn caught mid-spawn (before its skin is applied) is told apart from one that
        // never gets a body.
        void NoteBody(const std::string& who, APawn* p, const std::string& why) {
            static std::map<void*, unsigned long long> missingSince;   // pawn -> GetTickCount64()
            auto it = missingSince.find(p);
            if (why.empty()) {
                if (it != missingSince.end()) {
                    printf("[VISIBLE] %s's pawn %p (hero %s) has its body now, %.0f s after it was first seen without one\n",
                        who.c_str(), (void*)p, HeroOf(p).c_str(), (GetTickCount64() - it->second) / 1000.0);
                    missingSince.erase(it);
                }
                return;
            }
            if (it != missingSince.end()) return;
            missingSince[p] = GetTickCount64();
            USkeletalMeshComponent* m = Gone(p->Mesh) ? nullptr : p->Mesh;
            std::string skin = "not a PoplarPawn";
            if (p->IsA(APoplarPawn::StaticClass())) {
                APoplarPawn* pp = reinterpret_cast<APoplarPawn*>(p);
                skin = "skin rep " + NameOf(pp->SkinData.SkinDef) + ", applied skin " + NameOf(pp->AppliedSkin) +
                       ", skin definition " + NameOf(pp->SkinDefinition) + ", body class " + NameOf(pp->PoplarBodyClass);
            }
            printf("[VISIBLE] %s's pawn %p (hero %s, archetype %s) has no visible body: %s | mesh %s, skeletal mesh %s, attached %u, mesh hidden %u, pawn hidden %u, health %.0f | %s\n",
                who.c_str(), (void*)p, HeroOf(p).c_str(), NameOf(p->ObjectArchetype).c_str(), why.c_str(),
                m ? m->GetName().c_str() : "none", m ? NameOf(m->SkeletalMesh).c_str() : "none",
                m ? (unsigned)m->bAttached : 0u, m ? (unsigned)m->HiddenGame : 0u, (unsigned)p->bHidden,
                p->GetHealth(), skin.c_str());
        }

        std::string PlayerNameOf(APawn* p) {
            APlayerReplicationInfo* pri = p->PlayerReplicationInfo;
            std::string name = Gone(pri) ? std::string() : pri->PlayerName.ToString();
            return name.empty() ? std::string("unnamed") : name;
        }

        // --- Which hero is it, and does it wear its own skin? ------------------------------------
        //
        // Before co-op over the internet every hero has to load as the one picked, in its own skin
        // (s3 runs 20261006-023201, -025246: a ModernSoldier in RocketHawk's skin had no body).

        // Display name ("Oscar Mike") of a hero token ("ModernSoldier"), matched without case;
        // "" when the token is no known hero.
        std::string DisplayNameOf(const std::string& token) {
            if (token.empty()) return "";
            for (const auto& [cls, display] : Constants::CharacterLookupTable)
                if (_stricmp(cls.c_str(), token.c_str()) == 0) return display;
            return "";
        }

        // The hero the -rbcharacter option asks for as a display name (the autopilot also takes the
        // class token), the option as given when it names no known hero, "" when not set.
        std::string RequestedHero() {
            const std::string& want = LaunchOptions::Get().character;
            if (want.empty()) return "";
            for (const std::string& name : Constants::CharacterSelectCharacterTable)
                if (name == want) return name;
            std::string display = DisplayNameOf(want);
            return display.empty() ? want : display;
        }

        // A pawn's hero as a display name when its class maps to one ("Class_ModernSoldier" in
        // GD_ModernSoldier_Streaming -> "Oscar Mike"), else the class (or archetype) name.
        // known says whether it mapped.
        std::string PawnHero(APawn* p, bool& known) {
            known = false;
            if (p->IsA(APoplarPawn::StaticClass())) {
                UObject* def = reinterpret_cast<APoplarPawn*>(p)->PoplarPlayerClassDef;
                if (!Gone(def)) {
                    std::string name = def->GetName();
                    std::string display = DisplayNameOf(name.rfind("Class_", 0) == 0 ? name.substr(6) : name);
                    if (display.empty()) display = DisplayNameOf(Hooks::HeroTokenOf(def));
                    if (!display.empty()) { known = true; return display; }
                }
            }
            return HeroOf(p);
        }

        // "" when the pawn wears its own hero's skin (or it cannot tell), else which skin is
        // another known hero's: the skin definition, or the replicated skin (SkinData). Skins from
        // packages that name no hero are never counted.
        std::string WrongSkin(APawn* p) {
            if (!p->IsA(APoplarPawn::StaticClass())) return "";
            APoplarPawn* pp = reinterpret_cast<APoplarPawn*>(p);
            UObject* def = pp->PoplarPlayerClassDef;
            if (Gone(def)) return "";
            UObject* skins[] = { pp->SkinDefinition, pp->SkinData.SkinDef };
            for (UObject* skin : skins) {
                if (Gone(skin) || !Hooks::SkinIsOtherHeros(skin, def)) continue;
                std::string owner = DisplayNameOf(Hooks::HeroTokenOf(skin));
                if (owner.empty()) continue;
                return owner + "'s skin " + skin->GetName() + " on " + def->GetName();
            }
            return "";
        }

        // --- Computer-controlled characters and summons, for the server-vs-client check ---------
        //
        // Every networked non-player pawn (story enemies, lane minions, pets) and every networked
        // actor a player pawn instigated (summons, deployables, mines), one entry each. The test kit
        // (debugloop/npcsync.py) compares the server's list with each client's: a character the server
        // has near a player that the player lacks, one only the client has, or one without a body.
        // Client-only actors (effects the client spawns itself) are left out: on a client only
        // actors whose authority is the server (RemoteRole == Authority) count.
        constexpr size_t kMaxNpcs = 250;

        bool Networked(AActor* a) {
            return Globals::amServer ? a->RemoteRole != ENetRole::ROLE_None
                                     : a->RemoteRole == ENetRole::ROLE_Authority;
        }

        nlohmann::json IntLoc(AActor* a) {
            return { (int)a->Location.X, (int)a->Location.Y, (int)a->Location.Z };
        }

        bool IsPlayerPawn(APawn* p) {
            return !Gone(p) && !Gone(p->PlayerReplicationInfo);
        }

        nlohmann::json NpcCensus(int& pawnCount, int& thingCount, nlohmann::json& skipped) {
            nlohmann::json out = nlohmann::json::array();
            pawnCount = thingCount = 0;
            // Networked dynamic actors left out, as "reason: class" -> count, so a summon the list
            // misses can be found (npc_skipped in /state).
            std::map<std::string, int> skips;
            auto skip = [&](const char* why, AActor* a) { skips[std::string(why) + ": " + a->Class->GetName()]++; };
            for (AActor* a : SDKUtils::GetAllOfClass<AActor>()) {
                if (Gone(a) || IsDefault(a) || a->bDeleteMe || a->bStatic || !a->WorldInfo || !Networked(a))
                    continue;
                bool pawn = a->IsA(APawn::StaticClass());
                if (pawn && IsPlayerPawn(reinterpret_cast<APawn*>(a)))
                    continue;
                if (a->IsA(AInfo::StaticClass()) || a->IsA(AController::StaticClass()) ||
                    a->IsA(AInventory::StaticClass()) || a->IsA(APlayerStateInfo::StaticClass()))
                    continue;
                if (a->bTearOff) { skip("torn off", a); continue; }
                // Never sent to other players: owner-only actors and hidden ones (UE3 does not
                // replicate a hidden actor; hazard volumes are hidden pawns). A client's copy hidden
                // while the server's is not shows up as "missing" instead.
                if (a->bOnlyRelevantToOwner) { skip("owner only", a); continue; }
                if (a->bHidden) { skip("hidden", a); continue; }
                std::string owner;
                if (pawn) {
                    APawn* p = reinterpret_cast<APawn*>(a);
                    float hp = PawnUtils::MinionHealth(p, !Globals::amServer);   // -1 = alive, health unknown
                    if (!LivePawnInWorld(p) || hp == 0.0f) { skip("dead", a); continue; }
                    if (IsPlayerPawn(p->Instigator) && p->Instigator != p) owner = PlayerNameOf(p->Instigator);
                } else {
                    if (a->IsA(AEmitter::StaticClass())) { skip("effect", a); continue; }
                    // Only things players made: instigated by a player pawn, or owned by one.
                    APawn* by = IsPlayerPawn(a->Instigator) ? a->Instigator
                              : (a->Owner && a->Owner->IsA(APawn::StaticClass()) && IsPlayerPawn(reinterpret_cast<APawn*>(a->Owner)))
                                    ? reinterpret_cast<APawn*>(a->Owner) : nullptr;
                    if (!by) { skip("not a player's", a); continue; }
                    owner = PlayerNameOf(by);
                }
                (pawn ? pawnCount : thingCount)++;
                if (out.size() >= kMaxNpcs) continue;
                nlohmann::json e;
                e["k"] = pawn ? "pawn" : "thing";
                e["a"] = a->ObjectArchetype && !IsDefault(a->ObjectArchetype) ? a->ObjectArchetype->GetName() : a->Class->GetName();
                e["c"] = a->Class->GetName();
                e["p"] = IntLoc(a);
                if (!owner.empty()) e["own"] = owner;
                if (pawn) {
                    APawn* p = reinterpret_cast<APawn*>(a);
                    float hp = PawnUtils::MinionHealth(p, !Globals::amServer);
                    if (hp >= 0.0f) e["hp"] = (int)hp;   // left out: alive, health not known on this client
                    // A dedicated server draws nothing: only a missing body counts there.
                    std::string why = MissingBody(p, !Globals::amServer);
                    if (!why.empty()) e["body"] = why;
                }
                out.push_back(std::move(e));
            }
            std::vector<std::pair<int, std::string>> top;
            for (auto& [k, n] : skips) top.push_back({ -n, k });
            std::sort(top.begin(), top.end());
            skipped = nlohmann::json::object();
            for (size_t i = 0; i < top.size() && i < 25; i++) skipped[top[i].second] = -top[i].first;
            return out;
        }

        std::string DisconnectReason() {
            std::string line = Diagnostics::LastLineContaining("Failure");
            if (line.empty()) return "";
            if (line.find("Timeout") != std::string::npos || line.find("timed out") != std::string::npos) return "timeout";
            if (line.find("ConnectionLost") != std::string::npos) return "lost";
            if (line.find("PendingConnectionFailure") != std::string::npos) return "pending";
            return "other";
        }
    }

    void SetListening(bool listening) {
        g_listening = listening;
        // A server started by "Host Game" tells the host's game it can connect now (LocalHost.cpp).
        const std::wstring& name = LaunchOptions::Get().readyEvent;
        if (listening && !name.empty()) {
            HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str());
            std::printf("[HOST] listening; telling the host's game %s\n", ready ? "to join" : "failed (it has closed?)");
            if (ready) {
                SetEvent(ready);
                CloseHandle(ready);
            }
        }
    }

    std::string SnapshotJson() {
        const auto& opt = LaunchOptions::Get();
        nlohmann::json j;
        std::string role = Globals::amServer ? "server" : (Globals::amStandalone ? "solo" : "client");
        std::string map = MapName();
        PROCESS_MEMORY_COUNTERS pmc{};
        GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
        j["instance"] = std::string(opt.instance.begin(), opt.instance.end());
        j["role"] = role;
        j["pid"] = GetCurrentProcessId();
        j["ticks"] = Diagnostics::TickCount();
        j["map"] = map;
        j["net_mode"] = role == "server" ? "dedicated" : (role == "solo" ? "standalone" : "client");
        j["memory_mb"] = pmc.WorkingSetSize / (1024 * 1024);
        int matchState = MatchState();
        j["match_state"] = matchState;
        j["match_over"] = !Diagnostics::LastLineContaining("Match ended").empty() ||
                          matchState >= (int)EMatchState::MatchState_Ended;
        if (!Globals::amStandalone && Globals::GetGWorld()) {
            int pawns = 0, things = 0;
            nlohmann::json skipped;
            j["npcs"] = NpcCensus(pawns, things, skipped);
            j["npc_skipped"] = skipped;
            j["npc_counts"] = { {"pawns", pawns}, {"things", things} };
        }

        if (Globals::amServer) {
            nlohmann::json locs = nlohmann::json::object();
            nlohmann::json invisible = nlohmann::json::object();   // unique id -> what is missing
            int connections = 0;
            for (APoplarPlayerController* pc : SDKUtils::GetAllOfClass<APoplarPlayerController>()) {
                if (!pc || IsDefault(pc) || !pc->Player) continue;
                connections++;
                if (pc->Pawn && pc->PlayerReplicationInfo) {
                    std::string key = UniqueIdString(pc->PlayerReplicationInfo->UniqueId);
                    if (!key.empty()) locs[key] = Loc(pc->Pawn);
                    // A dedicated server draws nothing, so only a missing or detached body counts.
                    if (LivePawnInWorld(pc->Pawn)) {
                        std::string why = MissingBody(pc->Pawn, false);
                        NoteBody("server: player " + (key.empty() ? std::string("?") : key), pc->Pawn, why);
                        if (!why.empty()) invisible[key.empty() ? PlayerNameOf(pc->Pawn) : key] = why;
                    }
                }
            }
            j["listening"] = g_listening.load();
            j["connections"] = connections;
            j["network_objects"] = SDKUtils::GetAllOfClass<UActorChannel>().size();
            j["player_locations"] = locs;
            j["players_invisible"] = invisible;
        } else {
            APoplarPlayerController* pc = SDKUtils::GetLocalPlayerController();
            if (!pc) pc = SDKUtils::GetLastOfClass<APoplarPlayerController>();
            bool hasPawn = pc && !IsDefault(pc) && pc->Pawn;
            j["has_pawn"] = hasPawn;
            std::string uid = (pc && !IsDefault(pc) && pc->PlayerReplicationInfo)
                ? UniqueIdString(pc->PlayerReplicationInfo->UniqueId) : "";
            j["unique_id"] = uid.empty() ? nlohmann::json(nullptr) : nlohmann::json(uid);
            j["pawn_location"] = hasPawn ? Loc(pc->Pawn) : nlohmann::json(nullptr);
            j["pawn_health"] = hasPawn ? pc->Pawn->GetHealth() : 0.0f;
            j["combat"] = { {"enabled", Combat::Enabled()}, {"stats", Combat::StatsJson()} };

            // Own body: pawn_visible false (with pawn_body_missing saying why), null with no pawn.
            APawn* own = hasPawn && LivePawnInWorld(pc->Pawn) ? pc->Pawn : nullptr;
            if (own) {
                std::string why = MissingBody(own, false);
                NoteBody("local player", own, why);
                j["pawn_visible"] = why.empty();
                j["pawn_body_missing"] = why;
            } else {
                j["pawn_visible"] = nullptr;
                j["pawn_body_missing"] = nullptr;
            }
            // Own hero: requested_hero (the -rbcharacter pick), pawn_hero (what loaded) and
            // hero_matches (null with no pick, no pawn, or a hero that maps to no display name).
            // pawn_skin_ok false (pawn_skin_wrong says which) when it wears another hero's skin.
            std::string requested = RequestedHero();
            j["requested_hero"] = requested.empty() ? nlohmann::json(nullptr) : nlohmann::json(requested);
            j["pawn_hero"] = nullptr;
            j["hero_matches"] = nullptr;
            j["pawn_skin_ok"] = nullptr;
            j["pawn_skin_wrong"] = nullptr;
            if (own) {
                bool known = false;
                std::string hero = PawnHero(own, known);
                j["pawn_hero"] = hero;
                if (known && !requested.empty()) j["hero_matches"] = hero == requested;
                std::string wrong = WrongSkin(own);
                j["pawn_skin_ok"] = wrong.empty();
                if (!wrong.empty()) j["pawn_skin_wrong"] = wrong;
            }
            // Other living human players' pawns this client has, those without a visible body
            // ("name (hero): what is missing") and those in another hero's skin ("name (hero):
            // which skin"). Bots are left out.
            nlohmann::json othersInvisible = nlohmann::json::array();
            nlohmann::json othersWrongSkin = nlohmann::json::array();
            int othersSeen = 0;
            for (APoplarPlayerPawn* p : SDKUtils::GetAllOfClass<APoplarPlayerPawn>()) {
                if (!LivePawnInWorld(p) || (hasPawn && (APawn*)p == pc->Pawn) || !IsHumanPawn(p)) continue;
                if (p->GetHealth() <= 0.0f) continue;
                othersSeen++;
                std::string why = MissingBody(p, true);
                NoteBody("other player " + PlayerNameOf(p), p, why);
                if (!why.empty()) othersInvisible.push_back(PlayerNameOf(p) + " (" + HeroOf(p) + "): " + why);
                std::string wrong = WrongSkin(p);
                if (!wrong.empty()) othersWrongSkin.push_back(PlayerNameOf(p) + " (" + HeroOf(p) + "): " + wrong);
            }
            j["others_seen"] = othersSeen;
            j["others_invisible"] = othersInvisible;
            j["others_wrong_skin"] = othersWrongSkin;
            bool inMenu = map.empty() || map.find("MenuMap") != std::string::npos;
            j["connected"] = role == "client" ? !inMenu : true;
            j["disconnect_reason"] = DisconnectReason();
            j["autopilot"] = Autopilot::PhaseName();
        }
        return j.dump();
    }

    std::string Exec(const std::string& command) {
        std::wstring w(command.begin(), command.end());
        Engine::ExecConsoleCommand(w.c_str());
        return "ok";
    }
}

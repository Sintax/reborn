#include "GameState.hpp"
#include "Autopilot.hpp"
#include "Diagnostics.hpp"
#include "Engine.hpp"
#include "Globals.hpp"
#include "LaunchOptions.hpp"
#include "Utils.hpp"
#include "json.hpp"
#include <Windows.h>
#include <Psapi.h>
#include <atomic>
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

        bool IsDefault(UObject* o) { return o->GetFullName().find("Default__") != std::string::npos; }

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
        const uint64_t kPendingKill = 0x2000000000000000ull;

        bool Gone(UObject* o) { return !o || (o->ObjectFlags & kPendingKill); }

        // A live pawn in a level (not an archetype or class default, not being destroyed).
        bool LivePawnInWorld(APawn* p) {
            return !Gone(p) && !p->bDeleteMe && !p->bTearOff && p->Outer && p->Outer->IsA(ULevel::StaticClass());
        }

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

        std::string HeroOf(APawn* p) {
            if (p->IsA(APoplarPawn::StaticClass())) {
                UPoplarPlayerClassDefinition* def = reinterpret_cast<APoplarPawn*>(p)->PoplarPlayerClassDef;
                if (!Gone(def)) return def->GetName();
            }
            return !Gone(p->ObjectArchetype) ? p->ObjectArchetype->GetName() : std::string("unknown");
        }

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

        std::string DisconnectReason() {
            std::string line = Diagnostics::LastLineContaining("Failure");
            if (line.empty()) return "";
            if (line.find("Timeout") != std::string::npos || line.find("timed out") != std::string::npos) return "timeout";
            if (line.find("ConnectionLost") != std::string::npos) return "lost";
            if (line.find("PendingConnectionFailure") != std::string::npos) return "pending";
            return "other";
        }
    }

    void SetListening(bool listening) { g_listening = listening; }

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
            // Other living human players' pawns this client has, and those without a visible body
            // ("name (hero): what is missing"). Bots are left out.
            nlohmann::json othersInvisible = nlohmann::json::array();
            int othersSeen = 0;
            for (APoplarPlayerPawn* p : SDKUtils::GetAllOfClass<APoplarPlayerPawn>()) {
                if (!LivePawnInWorld(p) || (hasPawn && (APawn*)p == pc->Pawn) || !IsHumanPawn(p)) continue;
                if (p->GetHealth() <= 0.0f) continue;
                othersSeen++;
                std::string why = MissingBody(p, true);
                NoteBody("other player " + PlayerNameOf(p), p, why);
                if (!why.empty()) othersInvisible.push_back(PlayerNameOf(p) + " (" + HeroOf(p) + "): " + why);
            }
            j["others_seen"] = othersSeen;
            j["others_invisible"] = othersInvisible;
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

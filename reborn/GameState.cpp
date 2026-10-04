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
        j["match_over"] = !Diagnostics::LastLineContaining("Match ended").empty();

        if (Globals::amServer) {
            nlohmann::json locs = nlohmann::json::object();
            int connections = 0;
            for (APoplarPlayerController* pc : SDKUtils::GetAllOfClass<APoplarPlayerController>()) {
                if (!pc || IsDefault(pc) || !pc->Player) continue;
                connections++;
                if (pc->Pawn && pc->PlayerReplicationInfo) {
                    std::string key = UniqueIdString(pc->PlayerReplicationInfo->UniqueId);
                    if (!key.empty()) locs[key] = Loc(pc->Pawn);
                }
            }
            j["listening"] = g_listening.load();
            j["connections"] = connections;
            j["network_objects"] = SDKUtils::GetAllOfClass<UActorChannel>().size();
            j["player_locations"] = locs;
        } else {
            APoplarPlayerController* pc = SDKUtils::GetLastOfClass<APoplarPlayerController>();
            bool hasPawn = pc && !IsDefault(pc) && pc->Pawn;
            j["has_pawn"] = hasPawn;
            std::string uid = (pc && !IsDefault(pc) && pc->PlayerReplicationInfo)
                ? UniqueIdString(pc->PlayerReplicationInfo->UniqueId) : "";
            j["unique_id"] = uid.empty() ? nlohmann::json(nullptr) : nlohmann::json(uid);
            j["pawn_location"] = hasPawn ? Loc(pc->Pawn) : nlohmann::json(nullptr);
            j["pawn_health"] = hasPawn ? pc->Pawn->GetHealth() : 0.0f;
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

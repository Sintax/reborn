#include "PlayerNames.hpp"
#include "Globals.hpp"
#include "LaunchOptions.hpp"
#include "StartKey.hpp"
#include "Utils.hpp"
#include "safetyhook.hpp"
#include <cstdio>
#include <string>
#include <vector>

namespace PlayerNames {
    namespace {
        const uint64_t kPendingKill = 0x2000000000000000ull; // RF_PendingKill

        SafetyHookInline g_refreshName;

        bool Gone(UObject* o) { return !o || (o->ObjectFlags & kPendingKill); }

        bool SameText(const FString& a, const FString& b) {
            if (a.ArrayCount != b.ArrayCount) return false;
            if (a.ArrayCount == 0) return true;
            return a.ArrayData && b.ArrayData && wcscmp(a.ArrayData, b.ArrayData) == 0;
        }

        // FString::operator= of the engine (battleborn+0x39ec0), so the buffer comes from the
        // engine's string allocator (see the login code in Hooks.cpp). `count` includes the 0.
        void Assign(FString* to, const wchar_t* text, int count) {
            struct { const wchar_t* begin; const wchar_t* end; } range = { text, text + count };
            reinterpret_cast<void (*)(FString*, void*)>(Globals::baseAddress + 0x039ec0)(to, &range);
        }

        void Assign(FString* to, const std::string& text) {
            std::wstring w(text.begin(), text.end());   // cleaned names are plain ASCII
            Assign(to, w.c_str(), (int)w.size() + 1);
        }

        // The game's name refresh (battleborn+0x123ac90), run from every read of a player's name.
        // For a human it asks Steam for the name of the player's id and overwrites PlayerName.
        // Bots and solo games keep the game's own behaviour. Humans in a network game keep the
        // name the server set: it is copied into the HTML and short copies the screen reads.
        void RefreshNameHook(APlayerReplicationInfo* pri, void* uniqueId) {
            if (!pri || pri->bBot || !pri->WorldInfo || pri->WorldInfo->NetMode == ENetMode::NM_Standalone) {
                g_refreshName.call<void>(pri, uniqueId);
                return;
            }
            const FString& name = pri->PlayerName;
            if (name.ArrayCount <= 1 || !name.ArrayData) return;   // not replicated yet: stays dirty
            if (!pri->bPlayerNameDirty && SameText(name, pri->PlayerNameHTML)) return;

            Assign(&pri->PlayerNameHTML, name.ArrayData, name.ArrayCount);
            int shortLength = pri->PlayerDisplayNameTruncationLength > 0 ? pri->PlayerDisplayNameTruncationLength : 16;
            int length = name.ArrayCount - 1;
            if (length > shortLength) {
                std::wstring cut(name.ArrayData, shortLength);
                Assign(&pri->PlayerNameTruncated, cut.c_str(), (int)cut.size() + 1);
            }
            else {
                Assign(&pri->PlayerNameTruncated, name.ArrayData, name.ArrayCount);
            }
            Assign(&pri->PlayerNameTruncatedHTML, pri->PlayerNameTruncated.ArrayData, pri->PlayerNameTruncated.ArrayCount);
            pri->bPlayerNameDirty = 0;
        }

        std::string NamesSeen(AWorldInfo* wi) {
            std::string out;
            if (!wi || Gone(wi->GRI)) return out;
            int count = wi->GRI->PRIArray.ArrayCount;
            for (int i = 0; i < count && i < 64; i++) {
                APlayerReplicationInfo* pri = wi->GRI->PRIArray[i];
                if (Gone(pri) || pri->bBot || pri->bIsSpectator) continue;
                out += (out.empty() ? "" : ",") + pri->PlayerName.ToString();
            }
            return out;
        }
    }

    void InstallHooks() {
        g_refreshName = safetyhook::create_inline((void*)(Globals::baseAddress + 0x123ac90), &RefreshNameHook);
        if (!g_refreshName)
            printf("[STARTUP] hook FAILED: player name refresh (battleborn+0x123ac90)\n");
    }

    std::string ChosenName() {
        const std::string& test = LaunchOptions::Get().name;
        return CleanName(test.empty() ? StartKey::PlayerName() : test);
    }

    void ClientTick(float dt) {
        static APlayerController* sentTo = nullptr;
        static float sinceLog = 0.f;
        static std::string lastSeen;

        APoplarPlayerController* pc = SDKUtils::GetLocalPlayerController();
        if (!pc || !pc->WorldInfo || pc->WorldInfo->NetMode != ENetMode::NM_Client) {
            sentTo = nullptr;
            return;
        }

        sinceLog += dt;
        if (sinceLog >= 5.f) {
            sinceLog = 0.f;
            std::string seen = NamesSeen(pc->WorldInfo);
            if (seen != lastSeen) {
                printf("[NAMES] client: players %s\n", seen.c_str());
                lastSeen = seen;
            }
        }

        // Once per controller, as soon as the server has logged it in. The RPC is reliable.
        if (pc == sentTo || Gone(pc->PlayerReplicationInfo)) return;
        sentTo = pc;
        std::string name = ChosenName();
        std::wstring w(name.begin(), name.end());
        FString text;
        text.ArrayData = w.c_str();
        text.ArrayCount = text.ArrayMax = (int32_t)w.size() + 1;
        pc->ServerChangeName(text);   // copied into the RPC; the buffer stays ours
        printf("[NAMES] client: sent name \"%s\"\n", name.c_str());
    }

    void SetHumanName(APlayerController* pc, const std::string& wanted) {
        if (Gone(pc) || Gone(pc->PlayerReplicationInfo)) return;
        APlayerReplicationInfo* pri = pc->PlayerReplicationInfo;

        std::vector<std::string> taken;
        if (pc->WorldInfo && !Gone(pc->WorldInfo->GRI)) {
            int count = pc->WorldInfo->GRI->PRIArray.ArrayCount;
            for (int i = 0; i < count && i < 64; i++) {
                APlayerReplicationInfo* other = pc->WorldInfo->GRI->PRIArray[i];
                if (Gone(other) || other == pri || other->bBot) continue;
                taken.push_back(other->PlayerName.ToString());
            }
        }
        std::string name = UniqueName(CleanName(wanted), taken);

        Assign(&pri->PlayerName, name);
        pri->bPlayerNameDirty = 1;   // the refresh hook copies it into the other three
        // PlayerName replicates only with a PRI's first update (bNetInitial, see the PRI's
        // replication list at battleborn+0x39ecb0), so clients that already have this PRI keep
        // the old name; only players who arrive later see the new one.

        std::string was = "?";
        for (Globals::ServerPlayer& sp : Globals::ServerPlayers) {
            if (sp.Connection && (UPlayer*)sp.Connection == pc->Player) {
                was = sp.Name;
                sp.Name = name;
                break;
            }
        }
        printf("[NAMES] %s is now \"%s\" (asked for \"%s\")\n", was.c_str(), name.c_str(), wanted.c_str());
    }

    bool OnServerChangeName(UObject* object, UFunction* function, void* params) {
        static UFunction* changeName = nullptr;
        if (!changeName) changeName = UFunction::FindFunction("Function Engine.PlayerController.ServerChangeName");
        if (!changeName || function != changeName || !params || !object || !object->IsA(APlayerController::StaticClass()))
            return false;
        auto* p = reinterpret_cast<APlayerController_execServerChangeName_Params*>(params);
        SetHumanName(reinterpret_cast<APlayerController*>(object), p->S.ArrayCount > 0 ? p->S.ToString() : std::string());
        return true;
    }
}

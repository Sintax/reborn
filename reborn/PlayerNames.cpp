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
            // Not called: the original's name-changed virtual (PRI vtable +0xda8), which it runs only
            // on a client after a Steam lookup. The screens show the name without it (runs 20261018-190820, -191235).
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

    std::string ChosenName() {
        const std::string& test = LaunchOptions::Get().name;
        return CleanName(test.empty() ? StartKey::PlayerName() : test);
    }

    // Called once per join: the server sends Welcome once per connection. No "already sent"
    // memory, so a later join on a connection at a reused address still sends.
    void SendJoinName(UNetConnection* connection) {
        if (!connection) return;
        // The control channel (connection+0xf48), which the engine's send uses without a check.
        if (!*reinterpret_cast<void**>(reinterpret_cast<char*>(connection) + 0xf48)) {
            printf("[NAMES] client: no control channel at the welcome; the server will use its fallback name\n");
            return;
        }
        std::string name = ChosenName();
        std::wstring w = L"RebornName=" + std::wstring(name.begin(), name.end());
        FString text;
        text.ArrayData = w.c_str();
        text.ArrayCount = text.ArrayMax = (int32_t)w.size() + 1;
        // The engine's NMT_DebugText send (battleborn+0x3edc70, used by the NETDEBUGTEXT command):
        // it writes the text into a reliable control bunch, so the buffer stays ours.
        reinterpret_cast<void (*)(UNetConnection*, FString*)>(Globals::baseAddress + 0x3edc70)(connection, &text);
        printf("[NAMES] client: sent name \"%s\" with the join\n", name.c_str());
    }

    // The client's pending-level control-message handler (battleborn+0x4c2000, UNetPendingLevel::
    // NotifyControlMessage: 1 Welcome, 3 Challenge, 7 Uses, 0x13/0x15 the handshake). The mod's
    // server skips Challenge/Login, so the stock "?Name=" never goes out; once the server has
    // welcomed us, send the name ourselves. It arrives before NMT_Join, so before the login.
    SafetyHookInline g_pendingControlMessage;

    void PendingControlMessageHook(void* notify, UNetConnection* connection, uint8_t message, void* inbunch) {
        g_pendingControlMessage.call<void>(notify, connection, message, inbunch);
        if (message == 0x1)
            SendJoinName(connection);
    }

    void InstallHooks() {
        g_refreshName = safetyhook::create_inline((void*)(Globals::baseAddress + 0x123ac90), &RefreshNameHook);
        if (!g_refreshName)
            printf("[STARTUP] hook FAILED: player name refresh (battleborn+0x123ac90)\n");
        if (!Globals::amServer) {
            g_pendingControlMessage = safetyhook::create_inline((void*)(Globals::baseAddress + 0x4c2000), &PendingControlMessageHook);
            if (!g_pendingControlMessage)
                printf("[STARTUP] hook FAILED: pending-level control messages (battleborn+0x4c2000)\n");
        }
    }

    void OnJoinText(UNetConnection* connection, void* inbunch) {
        // FArchive << FString (battleborn+0x515b0), as the engine's own NMT_DebugText handler reads it.
        FString text;
        reinterpret_cast<void (*)(void*, FString*)>(Globals::baseAddress + 0x0515b0)(inbunch, &text);
        // By the count, not wcslen: the reader does not promise a final 0.
        std::string s;
        if (text.ArrayData && text.ArrayCount > 1)
            s = ToAscii(std::wstring(text.ArrayData, text.ArrayCount - 1));
        if (text.ArrayData)
            reinterpret_cast<void (*)(int, const void*)>(Globals::baseAddress + 0xd2e720)(2, text.ArrayData);   // the engine's free

        // Client text reaches the log only cleaned or made safe, and the odd lines are capped,
        // so a client cannot forge log lines or flood the log.
        static int oddLines = 0;
        const std::string key = "RebornName=";
        if (s.rfind(key, 0) != 0) {
            if (oddLines < 10) {
                oddLines++;
                printf("[NET] debug text from a client (%zu characters): %s\n", s.size(), SafeLogText(s).c_str());
            }
            return;
        }
        std::string wanted = CleanName(s.substr(key.size()));
        for (Globals::ServerPlayer& sp : Globals::ServerPlayers) {
            if (sp.Connection == connection) {
                static int nameLines = 0;
                if (sp.WantedName != wanted && nameLines < 50) {   // repeats and floods stay out of the log
                    nameLines++;
                    printf("[NAMES] %s asked to be \"%s\" (%zu characters sent)\n", SafeLogText(sp.Name).c_str(), wanted.c_str(), s.size() - key.size());
                }
                sp.WantedName = wanted;
                return;
            }
        }
        if (oddLines < 10) {
            oddLines++;
            printf("[NAMES] a connection with no player asked to be \"%s\"; ignored\n", wanted.c_str());
        }
    }

    void ClientTick(float dt) {
        static float sinceLog = 0.f;
        static std::string lastSeen;

        APoplarPlayerController* pc = SDKUtils::GetLocalPlayerController();
        if (!pc || !pc->WorldInfo || pc->WorldInfo->NetMode != ENetMode::NM_Client) return;

        sinceLog += dt;
        if (sinceLog < 5.f) return;
        sinceLog = 0.f;
        std::string seen = NamesSeen(pc->WorldInfo);
        if (seen != lastSeen) {
            printf("[NAMES] client: players %s\n", seen.c_str());
            lastSeen = seen;
        }
    }

    void SetHumanName(APlayerController* pc, std::string wanted) {   // a copy: callers pass ServerPlayer::Name, changed below
        if (Gone(pc) || Gone(pc->PlayerReplicationInfo)) return;
        APlayerReplicationInfo* pri = pc->PlayerReplicationInfo;

        std::vector<std::string> taken;
        if (pc->WorldInfo && !Gone(pc->WorldInfo->GRI)) {
            int count = pc->WorldInfo->GRI->PRIArray.ArrayCount;
            for (int i = 0; i < count && i < 64; i++) {
                APlayerReplicationInfo* other = pc->WorldInfo->GRI->PRIArray[i];
                if (Gone(other) || other == pri) continue;   // bots too: no human named like a bot
                taken.push_back(other->PlayerName.ToString());
            }
        }
        std::string name = UniqueName(CleanName(wanted), taken);

        Assign(&pri->PlayerName, name);
        pri->bPlayerNameDirty = 1;   // the refresh hook copies it into the other three
        // PlayerName replicates only with a PRI's first update (bNetInitial, see the PRI's
        // replication list at battleborn+0x39ecb0): a later change reaches only players who
        // arrive later, so this runs at login.

        std::string was = "?";
        for (Globals::ServerPlayer& sp : Globals::ServerPlayers) {
            if (sp.Connection && (UPlayer*)sp.Connection == pc->Player) {
                was = sp.Name;
                sp.Name = name;
                break;
            }
        }
        printf("[NAMES] %s is now \"%s\" (asked for \"%s\")\n", SafeLogText(was).c_str(), name.c_str(), SafeLogText(wanted).c_str());
    }
}

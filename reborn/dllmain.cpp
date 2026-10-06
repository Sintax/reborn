#include <thread>
#include <atomic>
#include <iostream>
#include <format>
#include <mutex>
#include <random>
#include <algorithm>
#include <execution>

#include "BB/SdkHeaders.hpp"
#include "safetyhook.hpp"
#include "httplib.h"
#include "LaunchOptions.hpp"
#include "Diagnostics.hpp"
#include "Combat.hpp"
#include "DebugServer.hpp"
#include "GameState.hpp"

#include "Init.hpp"
#include "Globals.hpp"
#include "Engine.hpp"

namespace {
    // Battleborn refuses to start ("An existing instance of the game is already running") while
    // another copy holds this mutex. Instances started by the debug loop (-rbinstance=NAME) each
    // get their own mutex, so a server and two clients can share one PC. Normal launches are untouched.
    constexpr const wchar_t* kSingleInstanceMutex = L"Shipping_Poplar_Client_Mutex";
    SafetyHookInline g_createMutexW;
    std::wstring g_instanceMutex;

    // What the game's own single-instance check got back (-1 = it has not run yet). If the mutex
    // already exists the game shows "already running" and quits before its first frame.
    std::atomic<long long> g_mutexError{ -1 };
    std::atomic<bool> g_mutexHandle{ false };

    HANDLE WINAPI CreateMutexWHook(LPSECURITY_ATTRIBUTES sa, BOOL initialOwner, LPCWSTR name) {
        bool ours = name && wcscmp(name, kSingleInstanceMutex) == 0;
        if (ours) name = g_instanceMutex.c_str();
        HANDLE h = g_createMutexW.call<HANDLE>(sa, initialOwner, name);
        DWORD err = GetLastError();
        if (ours) {
            g_mutexHandle = h != nullptr;
            g_mutexError = err;
        }
        SetLastError(err);   // the game reads ERROR_ALREADY_EXISTS right after this call
        return h;
    }

    // The game has exactly one "already running" check (checked in the binary): WinMain
    // (battleborn+0xdcc9c0) calls battleborn+0xdcc8b0, which does CreateMutexW on the name above
    // and stores the answer in GIsFirstInstance (battleborn+0x324350c, its only writer). PreInit
    // reads it at battleborn+0xdd1327 (its only reader) and shows Error_GameAlreadyRunning.
    // In run 20261005-020129 c2's mutex call never reached the rename hook, yet the answer was
    // "already running". So -rbinstance launches also guard the one place that acts on it.
    constexpr uintptr_t kFirstInstanceFlag = 0x324350c;    // GIsFirstInstance
    constexpr uintptr_t kAlreadyRunningCheck = 0xdd1327;  // PreInit: cmp byte [GIsFirstInstance], 0
    SafetyHookMid g_alreadyRunningGuard;
    std::atomic<bool> g_overrodeAlreadyRunning{ false };

    void AlreadyRunningGuard(safetyhook::Context&) {
        auto* firstInstance = reinterpret_cast<volatile uint8_t*>(
            reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + kFirstInstanceFlag);
        if (*firstInstance) {
            std::printf("[LAUNCH] the game's single-instance check passed (mutex %s)\n",
                        LaunchOptions::SingleInstanceMutexStatus().c_str());
            return;
        }
        *firstInstance = 1;
        g_overrodeAlreadyRunning = true;
        std::printf("[LAUNCH] the game's single-instance check said another copy is running (mutex %s); "
                    "ignoring it for this -rbinstance launch\n", LaunchOptions::SingleInstanceMutexStatus().c_str());
    }

    // Runs inside DllMain, before the game's WinMain makes its single-instance check.
    void AllowSeveralInstances() {
        std::wstring instance = LaunchOptions::InstanceArg(GetCommandLineW());
        if (instance.empty()) return;
        // The process id keeps a copy from the previous run that is still closing (the loop starts
        // the next run seconds later, same instance name) from blocking this one.
        g_instanceMutex = std::wstring(kSingleInstanceMutex) + L"_" + instance + L"_" +
                          std::to_wstring(GetCurrentProcessId());
        void* target = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateMutexW");
        if (target) g_createMutexW = safetyhook::create_inline(target, &CreateMutexWHook);

        // Only hook the expected instruction (80 3D <disp32> 00), so another game build is left alone.
        auto* check = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + kAlreadyRunningCheck);
        if (check[0] == 0x80 && check[1] == 0x3D && check[6] == 0x00 &&
            check + 7 + *reinterpret_cast<int32_t*>(check + 2) == check - kAlreadyRunningCheck + kFirstInstanceFlag)
            g_alreadyRunningGuard = safetyhook::create_mid(check, &AlreadyRunningGuard);
    }
}

std::string LaunchOptions::SingleInstanceMutexStatus() {
    if (g_instanceMutex.empty()) return "not renamed (no -rbinstance)";
    std::string name(g_instanceMutex.begin(), g_instanceMutex.end());
    long long err = g_mutexError;
    std::string s;
    if (err < 0) s = name + ": the game's mutex call never reached the mod's rename hook";
    else if (!g_mutexHandle) s = name + ": could not be created (error " + std::to_string(err) + ")";
    else if (err == ERROR_ALREADY_EXISTS) s = name + ": ALREADY EXISTED (another process holds it)";
    else s = name + ": created new";
    if (g_overrodeAlreadyRunning) s += "; the mod overrode the game's 'already running' answer";
    return s;
}

void MainThread() {
    Init::Globals();
    Init::Console();
    LaunchOptions::InitFromCommandLine();
    {
        const auto& opt = LaunchOptions::Get();
        Diagnostics::Init(opt.runDir, opt.instance, opt.hangSeconds);
        for (auto& e : opt.errors) std::printf("[LAUNCH] bad value: %ls\n", e.c_str());
        for (auto& u : opt.unknown) std::printf("[LAUNCH] unknown flag: %ls\n", u.c_str());
        if (opt.debugPort)
            std::printf("[LAUNCH] options: instance=\"%ls\" character=\"%s\" join=\"%ls\" solomap=\"%ls\" servermap=\"%ls\" players=%d seed=%u rundir=\"%ls\"\n",
                        opt.instance.c_str(), opt.character.c_str(), opt.join.c_str(), opt.soloMap.c_str(),
                        opt.serverMap.c_str(), opt.players, opt.seed, opt.runDir.c_str());
        if (!g_instanceMutex.empty())
            std::printf("[LAUNCH] single-instance mutex is %ls (rename hook %s, already-running guard %s)\n",
                        g_instanceMutex.c_str(), g_createMutexW ? "hooked" : "FAILED",
                        g_alreadyRunningGuard ? "hooked" : "FAILED");
        if (opt.debugPort) DebugServer::Start(opt.debugPort, DebugServer::Routes{
            GameState::SnapshotJson, GameState::Exec,
            [](const std::string&) { return Combat::SnapshotJson(); },
            Combat::ApplyOrderJson, Combat::ActJson });
    }
    Init::Hooks();

    while (!Globals::GetGWorld()) {
        if (Globals::amServer) {
            *(__int8*)(Globals::baseAddress + 0x3495af8) = 0; // GIsClient
            *(__int8*)(Globals::baseAddress + 0x348fad7) = 1; // GIsServer
        }
    }

    if (Globals::amServer) {
        Engine::DontPauseOnLossOfFocus();
    }
    else {
        Init::ImGUI();
    }
}

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        AllowSeveralInstances();
        std::thread t(MainThread);
        t.detach();
    }

    return TRUE;
}


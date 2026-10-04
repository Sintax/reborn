#include <thread>
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

    HANDLE WINAPI CreateMutexWHook(LPSECURITY_ATTRIBUTES sa, BOOL initialOwner, LPCWSTR name) {
        if (name && wcscmp(name, kSingleInstanceMutex) == 0) name = g_instanceMutex.c_str();
        return g_createMutexW.call<HANDLE>(sa, initialOwner, name);
    }

    // Runs inside DllMain, before the game's WinMain makes its single-instance check.
    void AllowSeveralInstances() {
        std::wstring instance = LaunchOptions::InstanceArg(GetCommandLineW());
        if (instance.empty()) return;
        g_instanceMutex = std::wstring(kSingleInstanceMutex) + L"_" + instance;
        void* target = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateMutexW");
        if (target) g_createMutexW = safetyhook::create_inline(target, &CreateMutexWHook);
    }
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
            std::printf("[LAUNCH] single-instance mutex is %ls (%s)\n", g_instanceMutex.c_str(),
                        g_createMutexW ? "hooked" : "hook FAILED");
        if (opt.debugPort) DebugServer::Start(opt.debugPort, GameState::SnapshotJson, GameState::Exec);
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


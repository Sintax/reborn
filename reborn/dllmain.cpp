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

void MainThread() {
    Init::Globals();
    Init::Console();
    LaunchOptions::InitFromCommandLine();
    {
        const auto& opt = LaunchOptions::Get();
        Diagnostics::Init(opt.runDir, opt.instance, opt.hangSeconds);
        for (auto& e : opt.errors) std::printf("[LAUNCH] bad value: %ls\n", e.c_str());
        for (auto& u : opt.unknown) std::printf("[LAUNCH] unknown flag: %ls\n", u.c_str());
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
        std::thread t(MainThread);
        t.detach();
    }

    return TRUE;
}


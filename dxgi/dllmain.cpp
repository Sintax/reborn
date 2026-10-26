// dllmain.cpp : Defines the entry point for the DLL application.
#include "pch.h"
#include <thread>
#include <iostream>
#include "dxgi.h"
#include "dxgi1_3.h"
#include "dxgi1_6.h"
#include "ModRollback.hpp"

#pragma comment(lib, "DXGI.lib")

HMODULE g_hOriginalDXGI = NULL;
typedef HRESULT(WINAPI* fpCreateDXGIFactory)(REFIID, void**);
typedef HRESULT(WINAPI* fpCreateDXGIFactory1)(REFIID, void**);
typedef HRESULT(WINAPI* fpCreateDXGIFactory2)(UINT, REFIID, void**);

fpCreateDXGIFactory g_pOrigCreateDXGIFactory = NULL;
fpCreateDXGIFactory1 g_pOrigCreateDXGIFactory1 = NULL;
fpCreateDXGIFactory2 g_pOrigCreateDXGIFactory2 = NULL;

void InitializeOriginalFunctions() {
    char systemPath[MAX_PATH];
    GetSystemDirectoryA(systemPath, MAX_PATH);

    char dxgiPath[MAX_PATH];
    sprintf_s(dxgiPath, MAX_PATH, "%s\\dxgi.dll", systemPath);

    g_hOriginalDXGI = LoadLibraryA(dxgiPath);
    if (g_hOriginalDXGI) {
        g_pOrigCreateDXGIFactory = (fpCreateDXGIFactory)GetProcAddress(g_hOriginalDXGI, "CreateDXGIFactory");
        g_pOrigCreateDXGIFactory1 = (fpCreateDXGIFactory1)GetProcAddress(g_hOriginalDXGI, "CreateDXGIFactory1");
        g_pOrigCreateDXGIFactory2 = (fpCreateDXGIFactory2)GetProcAddress(g_hOriginalDXGI, "CreateDXGIFactory2");
    }
}

extern "C" HRESULT __stdcall CreateDXGIFactoryFake(REFIID riid, void** ppFactory) {
    if (g_pOrigCreateDXGIFactory) {
        return g_pOrigCreateDXGIFactory(riid, ppFactory);
    }
    return E_FAIL;
}

extern "C" HRESULT __stdcall CreateDXGIFactory1Fake(REFIID riid, void** ppFactory) {
    if (g_pOrigCreateDXGIFactory1) {
        return g_pOrigCreateDXGIFactory1(riid, ppFactory);
    }
    return E_FAIL;
}

extern "C" HRESULT __stdcall CreateDXGIFactory2Fake(UINT Flags, REFIID riid, void** ppFactory) {
    if (g_pOrigCreateDXGIFactory2) {
        return g_pOrigCreateDXGIFactory2(Flags, riid, ppFactory);
    }
    return E_FAIL;
}

// The game's Win64 folder (where Battleborn.exe, this dxgi.dll and reborn.dll live).
static std::wstring GameFolder() {
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    std::wstring dir(exe, n);
    size_t slash = dir.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : dir.substr(0, slash);
}

void Main() {
    InitializeOriginalFunctions();

    if (LoadLibraryA("reborn.dll")) return;

    // A player update that does not load: go back to the mod it replaced (kept as reborn.dll.old).
    DWORD err = GetLastError();
    std::wstring win64 = GameFolder();
    if (win64.empty() || !ModRollback::PutBackPreviousMod(win64)) return;
    char note[160];
    sprintf_s(note, "[UPDATE] reborn.dll did not load (error %lu); put the previous version back\n", err);
    OutputDebugStringA(note);
    LoadLibraryW((win64 + L"\\reborn.dll").c_str());
}

BOOL APIENTRY DllMain(HMODULE hModule,
    DWORD  ul_reason_for_call,
    LPVOID lpReserved
)
{
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        Main();
    }
    return TRUE;
}
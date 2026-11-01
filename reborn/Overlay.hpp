#pragma once

#include <Windows.h>

#include "imgui/imgui.h"
#include "imgui/imgui_stdlib.h"

#include "Metagame.hpp"
#include "StartGameWindow.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace Overlay {
    // kind: the "Public" it was opened from; "Start a Game" then offers only those maps.
    void OpenServerBrowser(GameKind kind = GameKind::All);

    void OpenDirectConnect();

    void StartLaunchSequence(const wchar_t* command);

    void OpenSoloVSAI();

    void OpenCampaign();

    void OpenOperations();

    void OpenSaveManager();

    std::string GetItemDisplayName(const Metagame::Item* item);

    void Render();

    // The player pressed OK on the server's refusal notice (the autopilot does the same).
    void AcknowledgeRefusal();
}
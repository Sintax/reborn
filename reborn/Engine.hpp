#pragma once

#include "BB/SdkHeaders.hpp"
#include "Globals.hpp"
#include "Utils.hpp"

#include <functional>

namespace Engine {
    void* EngineMalloc(size_t size);

    // Runs fn on the game thread once `seconds` of game ticks have passed. Engine objects must only
    // be touched from the game thread: a detached std::thread that slept 3 s and then called
    // ProcessEvent on a PlayerController found it already freed, crashed inside the ProcessEvent
    // hook while holding the hook's mutex, and the game thread then waited on that mutex forever.
    void RunOnGameThreadAfter(float seconds, std::function<void()> fn);

    // Called once per engine tick from the game thread; runs the tasks that are due.
    void PumpGameThreadTasks(float dt);

    UObject* StaticConstructObject(UClass* theClass, UObject* outer);

    UObject* ScuffedDuplicateObject(UObject* InObject, UObject* Outer);

    void ExecConsoleCommand(const wchar_t* command);

    void DontPauseOnLossOfFocus();

    FString* MakeFString(const wchar_t* contents);

    void* ScaleformMalloc(unsigned int size);
}
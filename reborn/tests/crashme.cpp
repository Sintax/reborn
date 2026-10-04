// Usage: crashme <mode> <rundir>   mode = crash | hang | spam
#include "../Diagnostics.hpp"
#include <Windows.h>
#include <cstdio>
#include <string>

template <int N> __declspec(noinline) void Fault() { *(volatile int*)(uintptr_t)(N + 8) = N; }

template <int N> void TryFault() {
    __try { Fault<N>(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return 2;
    std::wstring mode = argv[1];
    Diagnostics::Init(argv[2], L"crashme", 5);
    Diagnostics::NoteTick();
    std::printf("hello from crashme\n");
    if (mode == L"crash") {
        Fault<0>();
    } else if (mode == L"hang") {
        Sleep(INFINITE);
    } else if (mode == L"spam") {
        TryFault<1>(); TryFault<1>(); TryFault<1>();   // same address: reported once
        TryFault<2>(); TryFault<3>(); TryFault<4>(); TryFault<5>();
        TryFault<6>(); TryFault<7>(); TryFault<8>();  // over the limit of 5
        Sleep(500);
        return 0;
    }
    return 3;
}

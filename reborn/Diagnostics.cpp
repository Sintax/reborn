#include "Diagnostics.hpp"
#include <Windows.h>
#include <DbgHelp.h>
#include <io.h>
#include <fcntl.h>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#pragma comment(lib, "dbghelp.lib")

namespace Diagnostics {
    namespace {
        std::wstring g_runDir, g_instance;
        int g_hangSeconds = 60;
        std::atomic<long long> g_lastTickMs{ 0 };
        std::atomic<unsigned long long> g_ticks{ 0 };
        std::atomic<DWORD> g_gameThreadId{ 0 };
        std::atomic<DWORD> g_workerThreadId{ 0 };

        std::mutex g_linesMutex;
        std::deque<std::string> g_lines;
        constexpr size_t kMaxLines = 300;

        // First-chance reports are deduplicated by address in a lock-free slot table (no heap, no mutex
        // on the faulting thread). The last of kMaxReports is kept for the final, unhandled report.
        constexpr int kMaxReports = 5;
        constexpr int kFirstChanceSlots = kMaxReports - 1;
        std::atomic<unsigned long long> g_slots[kFirstChanceSlots];
        std::atomic<int> g_reports{ 0 };
        std::atomic<DWORD> g_watchdogThreadId{ 0 };

        // The faulting thread copies the exception into these globals before handing off, so the
        // worker never reads memory owned by the faulting thread's stack.
        struct CrashRequest { DWORD threadId; bool firstChance; bool withMessage; unsigned seq; unsigned long long ticks; };
        CrashRequest g_request{};
        std::atomic<bool> g_finalReported{ false };   // at most one final (first_chance=false) report per process

        // UE3's fatal-error path (appError / a failed check or assert) writes the message into the engine's
        // error buffer, then raises this code. The game's own handlers catch it and show the "%s" dialog,
        // so it never becomes a second-chance exception and UnhandledFilter never runs.
        constexpr DWORD kEngineFatalError = 0xDEAD;
        const wchar_t* g_fatalTextSource = nullptr;
        size_t g_fatalTextMax = 0;
        char g_fatalText[2048];   // narrow, one line; filled on the faulting thread (no heap)
        EXCEPTION_RECORD g_recordCopy;
        CONTEXT g_contextCopy;
        EXCEPTION_POINTERS g_pointersCopy;
        std::atomic<bool> g_busy{ false };      // a request is owned by the worker
        std::atomic<unsigned> g_doneSeq{ 0 };
        unsigned g_nextSeq = 0;
        HANDLE g_requestEvent = nullptr, g_doneEvent = nullptr;
        LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;

        long long NowMs() { return (long long)GetTickCount64(); }
        const long long g_loadMs = NowMs();   // set when the DLL loads, before the game's WinMain

        std::string Narrow(const std::wstring& w) {
            if (w.empty()) return {};
            int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
            std::string s(n, '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
            return s;
        }

        std::string JsonString(const std::string& s) {
            std::string o = "\"";
            for (unsigned char c : s) {
                if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
                else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += (char)c;
            }
            return o + "\"";
        }

        void PushLine(const std::string& line) {
            std::lock_guard lk(g_linesMutex);
            g_lines.push_back(line);
            if (g_lines.size() > kMaxLines) g_lines.pop_front();
        }

        // Everything printed to stdout/stderr goes to the console, <instance>.log and the ring buffer.
        void StartLogTee() {
            int originalOut = _dup(_fileno(stdout));
            int fds[2];
            if (_pipe(fds, 1 << 20, _O_BINARY) != 0) return;
            std::fflush(stdout);
            std::fflush(stderr);
            _dup2(fds[1], _fileno(stdout));
            _dup2(fds[1], _fileno(stderr));
            std::setvbuf(stdout, nullptr, _IONBF, 0);
            std::setvbuf(stderr, nullptr, _IONBF, 0);
            std::wstring logPath = g_runDir + L"\\" + g_instance + L".log";
            std::thread([readFd = fds[0], originalOut, logPath] {
                FILE* log = _wfopen(logPath.c_str(), L"ab");
                char buf[4096];
                std::string partial;
                for (;;) {
                    int n = _read(readFd, buf, sizeof buf);
                    if (n <= 0) break;
                    if (originalOut >= 0) _write(originalOut, buf, n);
                    if (log) { std::fwrite(buf, 1, n, log); std::fflush(log); }
                    partial.append(buf, n);
                    size_t nl;
                    while ((nl = partial.find('\n')) != std::string::npos) {
                        std::string line = partial.substr(0, nl);
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        PushLine(line);
                        partial.erase(0, nl + 1);
                    }
                }
            }).detach();
        }

        // Raw x64 unwind. No heap use; guarded because the stack may be corrupt.
        int WalkStack(CONTEXT ctx, DWORD64* out, int max) {
            int n = 0;
            __try {
                while (n < max && ctx.Rip) {
                    out[n++] = ctx.Rip;
                    DWORD64 imageBase = 0;
                    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
                    if (!fn) {
                        ctx.Rip = *(DWORD64*)ctx.Rsp;
                        ctx.Rsp += 8;
                    } else {
                        PVOID handlerData = nullptr;
                        DWORD64 establisher = 0;
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx,
                                         &handlerData, &establisher, nullptr);
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            return n;
        }

        std::string FramesJson(const DWORD64* frames, int n) {
            std::string s = "[";
            for (int i = 0; i < n; i++) {
                if (i) s += ",";
                s += JsonString(FormatAddress(frames[i]));
            }
            return s + "]";
        }

        std::string TailJson() {
            // try_lock: the worker must never block on a lock the faulting thread might hold.
            std::unique_lock lk(g_linesMutex, std::try_to_lock);
            if (!lk.owns_lock()) return "[]";
            std::vector<std::string> lines(g_lines.begin(), g_lines.end());
            lk.unlock();
            size_t start = lines.size() > 50 ? lines.size() - 50 : 0;
            std::string s = "[";
            for (size_t i = start; i < lines.size(); i++) {
                if (i > start) s += ",";
                s += JsonString(lines[i]);
            }
            return s + "]";
        }

        void WriteDump(const std::wstring& path, EXCEPTION_POINTERS* ep, DWORD threadId) {
            HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE) return;
            MINIDUMP_EXCEPTION_INFORMATION mei{ threadId, ep, FALSE };
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory |
                                MiniDumpWithUnloadedModules),
                ep ? &mei : nullptr, nullptr, nullptr);
            CloseHandle(f);
        }

        void WriteJson(const std::wstring& path, const std::string& body) {
            std::wstring tmp = path + L".tmp";
            { std::ofstream o(tmp, std::ios::binary); o << body; }
            MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
        }

        bool IsFatal(DWORD code) {
            switch (code) {
            case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_ILLEGAL_INSTRUCTION:
            case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_STACK_OVERFLOW:
            case EXCEPTION_PRIV_INSTRUCTION: case EXCEPTION_IN_PAGE_ERROR:
            case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: case 0xC0000374 /* heap corruption */:
            case 0xC0000409 /* stack buffer overrun */:
                return true;
            }
            return false;
        }

        void HandleCrash(const CrashRequest& r) {
            EXCEPTION_POINTERS* ep = &g_pointersCopy;
            int index = ++g_reports;
            std::wstring base = g_runDir + L"\\" + g_instance + L"." + std::to_wstring(index);
            WriteDump(base + L".dmp", ep, r.threadId);
            DWORD64 frames[32];
            int n = WalkStack(*ep->ContextRecord, frames, 32);
            char code[16];
            std::snprintf(code, sizeof code, "0x%08lX", ep->ExceptionRecord->ExceptionCode);
            std::ostringstream j;
            j << "{\"code\":\"" << code << "\""
              << ",\"address\":" << JsonString(FormatAddress((DWORD64)ep->ExceptionRecord->ExceptionAddress))
              << ",\"frames\":" << FramesJson(frames, n)
              << ",\"thread\":" << r.threadId
              << ",\"game_thread\":" << (r.threadId == g_gameThreadId ? "true" : "false")
              << ",\"first_chance\":" << (r.firstChance ? "true" : "false")
              << ",\"ticks\":" << r.ticks
              << ",\"dump\":" << JsonString(Narrow(g_instance) + "." + std::to_string(index) + ".dmp");
            if (r.withMessage) j << ",\"message\":" << JsonString(g_fatalText);
            j << ",\"log_tail\":" << TailJson() << "}";
            WriteJson(base + L".crash.json", j.str());
        }

        void CrashWorker() {
            for (;;) {
                WaitForSingleObject(g_requestEvent, INFINITE);
                CrashRequest r = g_request;
                HandleCrash(r);
                g_doneSeq = r.seq;
                g_busy = false;
                SetEvent(g_doneEvent);
            }
        }

        // The dump is written from a separate thread: MiniDumpWriteDump is unreliable on the faulting thread.
        // If the wait times out the request is abandoned, but the worker only uses the global copies.
        void Report(EXCEPTION_POINTERS* ep, bool firstChance, bool withMessage = false) {
            static std::mutex oneAtATime;
            const unsigned long long ticksAtFault = g_ticks;   // before any wait below
            DWORD tid = GetCurrentThreadId();
            if (tid == g_workerThreadId || tid == g_watchdogThreadId) return;
            std::lock_guard lk(oneAtATime);
            ULONGLONG deadline = GetTickCount64() + 30000;
            bool expected = false;
            while (!g_busy.compare_exchange_strong(expected, true)) {   // previous request still running
                if (GetTickCount64() > deadline) return;
                Sleep(10);
                expected = false;
            }
            g_recordCopy = *ep->ExceptionRecord;
            g_contextCopy = *ep->ContextRecord;
            // The copy has no extended-state area, so drop the flag that says it does.
            g_contextCopy.ContextFlags &= ~(CONTEXT_XSTATE & ~CONTEXT_AMD64);
            g_pointersCopy = { &g_recordCopy, &g_contextCopy };
            unsigned seq = ++g_nextSeq;
            g_request = { tid, firstChance, withMessage, seq, ticksAtFault };
            SetEvent(g_requestEvent);
            while (g_doneSeq != seq) {
                ULONGLONG now = GetTickCount64();
                if (now >= deadline) return;
                WaitForSingleObject(g_doneEvent, (DWORD)(deadline - now));
            }
        }

        // Copies the engine's fatal message into g_fatalText as one line. No heap: runs on the faulting thread.
        void CopyFatalText() {
            g_fatalText[0] = '\0';
            if (!g_fatalTextSource || !g_fatalTextMax) return;
            __try {
                // At most 600 chars: up to 3 UTF-8 bytes each still fits, and a too-small buffer would fail outright.
                size_t n = wcsnlen(g_fatalTextSource, g_fatalTextMax < 600 ? g_fatalTextMax : 600);
                int len = n ? WideCharToMultiByte(CP_UTF8, 0, g_fatalTextSource, (int)n, g_fatalText,
                                                  (int)sizeof g_fatalText - 1, nullptr, nullptr) : 0;
                g_fatalText[len > 0 ? len : 0] = '\0';
            } __except (EXCEPTION_EXECUTE_HANDLER) { g_fatalText[0] = '\0'; }
            for (char* c = g_fatalText; *c; c++) if (*c == '\r' || *c == '\n' || *c == '\t') *c = ' ';
        }

        // The engine stops for good after raising 0xDEAD (it shows its dialog and then exits or sits there),
        // so this is the final report for the process, with the engine's own message attached.
        void ReportEngineFatalError(EXCEPTION_POINTERS* ep) {
            DWORD tid = GetCurrentThreadId();
            if (tid == g_workerThreadId || tid == g_watchdogThreadId) return;
            if (g_finalReported.exchange(true)) return;
            CopyFatalText();
            std::printf("[FATAL] the game hit a fatal error (exception 0xDEAD) on the %s thread %lu: %s\n",
                        tid == g_gameThreadId ? "game" : "non-game", tid,
                        g_fatalText[0] ? g_fatalText : "(engine error text not available)");
            Report(ep, false, true);
        }

        // No heap allocation and no locks here: this runs on the faulting thread.
        LONG CALLBACK VectoredHandler(EXCEPTION_POINTERS* ep) {
            if (ep->ExceptionRecord->ExceptionCode == kEngineFatalError) {
                ReportEngineFatalError(ep);
                return EXCEPTION_CONTINUE_SEARCH;
            }
            if (!IsFatal(ep->ExceptionRecord->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
            DWORD tid = GetCurrentThreadId();
            if (tid == g_workerThreadId || tid == g_watchdogThreadId) return EXCEPTION_CONTINUE_SEARCH;
            // Slots hold address+1 so that 0 can mean "empty" and a fault at address 0 is still deduplicated.
            unsigned long long addr = (unsigned long long)ep->ExceptionRecord->ExceptionAddress + 1;
            bool claimed = false;
            for (auto& slot : g_slots) {
                unsigned long long expected = 0;
                if (slot.compare_exchange_strong(expected, addr)) { claimed = true; break; }
                if (expected == addr) return EXCEPTION_CONTINUE_SEARCH;   // already reported
            }
            if (!claimed) return EXCEPTION_CONTINUE_SEARCH;               // first-chance slots used up
            Report(ep, true);
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // Always writes its own final report (first_chance=false), even for an address already reported.
        LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* ep) {
            if (!g_finalReported.exchange(true)) Report(ep, false);   // one final report per process
            return g_previousFilter ? g_previousFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
        }

        void Watchdog() {
            g_watchdogThreadId = GetCurrentThreadId();
            for (;;) {
                Sleep(1000);
                if (g_ticks == 0 || MillisSinceLastTick() < g_hangSeconds * 1000LL) continue;
                DWORD64 frames[32];
                int n = 0;
                HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                       FALSE, g_gameThreadId);
                if (th) {
                    SuspendThread(th);
                    CONTEXT ctx{};
                    ctx.ContextFlags = CONTEXT_FULL;
                    if (GetThreadContext(th, &ctx)) n = WalkStack(ctx, frames, 32);
                    ResumeThread(th);
                    CloseHandle(th);
                }
                std::wstring base = g_runDir + L"\\" + g_instance + L".hang";
                WriteDump(base + L".dmp", nullptr, 0);
                std::ostringstream j;
                j << "{\"seconds\":" << g_hangSeconds << ",\"frames\":" << FramesJson(frames, n)
                  << ",\"dump\":" << JsonString(Narrow(g_instance) + ".hang.dmp")
                  << ",\"log_tail\":" << TailJson() << "}";
                WriteJson(base + L".json", j.str());
                std::printf("[DIAG] game thread frozen for %d s; wrote hang report\n", g_hangSeconds);
                return;   // one hang report per process
            }
        }

        std::wstring DefaultRunDir() {
            wchar_t* profile = nullptr;
            size_t len = 0;
            _wdupenv_s(&profile, &len, L"USERPROFILE");
            std::wstring root = profile ? profile : L".";
            free(profile);
            wchar_t stamp[32];
            std::time_t t = std::time(nullptr);
            std::tm tm{};
            localtime_s(&tm, &t);
            std::wcsftime(stamp, 32, L"%Y%m%d-%H%M%S", &tm);
            return root + L"\\Documents\\RebornLogs\\" + g_instance + L"-" + stamp;
        }
    }

    void Init(const std::wstring& runDir, const std::wstring& instance, int hangSeconds) {
        g_instance = instance;
        g_hangSeconds = hangSeconds;
        g_runDir = runDir.empty() ? DefaultRunDir() : runDir;
        std::error_code ec;
        std::filesystem::create_directories(g_runDir, ec);
        StartLogTee();
        g_requestEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_doneEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        std::thread worker(CrashWorker);
        g_workerThreadId = GetThreadId(worker.native_handle());
        worker.detach();
        AddVectoredExceptionHandler(0, VectoredHandler);   // 0 = run after other handlers
        g_previousFilter = SetUnhandledExceptionFilter(UnhandledFilter);
        std::thread(Watchdog).detach();
        std::printf("[DIAG] run dir %ls, instance %ls, freeze limit %d s\n",
                    g_runDir.c_str(), g_instance.c_str(), g_hangSeconds);
    }

    void SetFatalErrorBuffer(const wchar_t* buffer, size_t maxChars) {
        g_fatalTextMax = maxChars;
        g_fatalTextSource = buffer;
    }

    void NoteTick() {
        DWORD expected = 0;
        g_gameThreadId.compare_exchange_strong(expected, GetCurrentThreadId());
        g_lastTickMs = NowMs();
        if (++g_ticks == 1)
            std::printf("[STARTUP] first engine frame done %.1f s after the mod loaded\n", (g_lastTickMs - g_loadMs) / 1000.0);
    }

    long long MillisSinceLastTick() {
        long long last = g_lastTickMs;
        return last == 0 ? 0 : NowMs() - last;
    }

    unsigned long long TickCount() { return g_ticks; }

    std::vector<std::string> RecentLines() {
        std::lock_guard lk(g_linesMutex);
        return { g_lines.begin(), g_lines.end() };
    }

    std::string LastLineContaining(const std::string& needle) {
        std::lock_guard lk(g_linesMutex);
        for (auto it = g_lines.rbegin(); it != g_lines.rend(); ++it)
            if (it->find(needle) != std::string::npos) return *it;
        return {};
    }

    std::string FormatAddress(unsigned long long address) {
        HMODULE mod = nullptr;
        char buf[64];
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCWSTR)address, &mod) || !mod) {
            std::snprintf(buf, sizeof buf, "0x%llx", address);
            return buf;
        }
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(mod, path, MAX_PATH);
        std::string name = Narrow(std::filesystem::path(path).stem().wstring());
        for (auto& c : name) c = (char)tolower((unsigned char)c);
        std::snprintf(buf, sizeof buf, "+0x%llx", address - (unsigned long long)mod);
        return name + buf;
    }

    const std::wstring& RunDir() { return g_runDir; }
}

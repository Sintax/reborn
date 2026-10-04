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
#include <set>
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
        DWORD g_workerThreadId = 0;

        std::mutex g_linesMutex;
        std::deque<std::string> g_lines;
        constexpr size_t kMaxLines = 300;

        std::mutex g_crashMutex;
        std::set<unsigned long long> g_seenAddresses;
        int g_reports = 0;
        constexpr int kMaxReports = 5;

        struct CrashRequest { EXCEPTION_POINTERS* ep; DWORD threadId; bool firstChance; };
        CrashRequest g_request{};
        HANDLE g_requestEvent = nullptr, g_doneEvent = nullptr;
        LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;

        long long NowMs() { return (long long)GetTickCount64(); }

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
            auto lines = RecentLines();
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
            int index;
            {
                std::lock_guard lk(g_crashMutex);
                index = ++g_reports;
            }
            std::wstring base = g_runDir + L"\\" + g_instance + L"." + std::to_wstring(index);
            WriteDump(base + L".dmp", r.ep, r.threadId);
            DWORD64 frames[32];
            int n = WalkStack(*r.ep->ContextRecord, frames, 32);
            char code[16];
            std::snprintf(code, sizeof code, "0x%08lX", r.ep->ExceptionRecord->ExceptionCode);
            std::ostringstream j;
            j << "{\"code\":\"" << code << "\""
              << ",\"address\":" << JsonString(FormatAddress((DWORD64)r.ep->ExceptionRecord->ExceptionAddress))
              << ",\"frames\":" << FramesJson(frames, n)
              << ",\"thread\":" << r.threadId
              << ",\"game_thread\":" << (r.threadId == g_gameThreadId ? "true" : "false")
              << ",\"first_chance\":" << (r.firstChance ? "true" : "false")
              << ",\"dump\":" << JsonString(Narrow(g_instance) + "." + std::to_string(index) + ".dmp")
              << ",\"log_tail\":" << TailJson() << "}";
            WriteJson(base + L".crash.json", j.str());
        }

        void CrashWorker() {
            for (;;) {
                WaitForSingleObject(g_requestEvent, INFINITE);
                HandleCrash(g_request);
                SetEvent(g_doneEvent);
            }
        }

        // The dump is written from a separate thread: MiniDumpWriteDump is unreliable on the faulting thread.
        void Report(EXCEPTION_POINTERS* ep, bool firstChance) {
            static std::mutex oneAtATime;
            if (GetCurrentThreadId() == g_workerThreadId) return;
            std::lock_guard lk(oneAtATime);
            g_request = { ep, GetCurrentThreadId(), firstChance };
            SetEvent(g_requestEvent);
            WaitForSingleObject(g_doneEvent, 30000);
        }

        LONG CALLBACK VectoredHandler(EXCEPTION_POINTERS* ep) {
            if (!IsFatal(ep->ExceptionRecord->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
            {
                std::lock_guard lk(g_crashMutex);
                if (g_reports >= kMaxReports) return EXCEPTION_CONTINUE_SEARCH;
                if (!g_seenAddresses.insert((unsigned long long)ep->ExceptionRecord->ExceptionAddress).second)
                    return EXCEPTION_CONTINUE_SEARCH;
            }
            Report(ep, true);
            return EXCEPTION_CONTINUE_SEARCH;
        }

        LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* ep) {
            bool room;
            {
                std::lock_guard lk(g_crashMutex);
                room = g_reports < kMaxReports + 1;   // always room for the final, second-chance report
            }
            if (room) Report(ep, false);
            return g_previousFilter ? g_previousFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
        }

        void Watchdog() {
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

    void NoteTick() {
        if (g_gameThreadId == 0) g_gameThreadId = GetCurrentThreadId();
        g_lastTickMs = NowMs();
        ++g_ticks;
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

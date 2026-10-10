#include "LocalHost.hpp"

#include <Windows.h>
#include <TlHelp32.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

namespace LocalHost {
    namespace {
        constexpr const wchar_t* kInstance = L"host-server";
        constexpr const char* kSteamId = "76561197960342817";   // any id that is not the host's own
        constexpr ULONGLONG kReadyTimeoutMs = 5 * 60 * 1000;    // a big map on a slow disk takes ~2 min
        const wchar_t* const kLoaderFiles[] = { L"steamclient_loader_x64.exe", L"steamclient.dll", L"steamclient64.dll" };

        std::mutex g_mu;                    // guards everything below
        Status g_status = Status::Idle;
        std::string g_message;
        HANDLE g_job = nullptr;             // the running server's job; closing it closes the server
        unsigned g_generation = 0;          // bumped by every Start and Stop; a stale worker gives up

        void Set(unsigned gen, Status s, const std::string& message) {
            std::lock_guard lock(g_mu);
            if (gen != g_generation) return;
            g_status = s;
            g_message = message;
            std::printf("[HOST] %s\n", message.c_str());
        }

        bool Current(unsigned gen) {
            std::lock_guard lock(g_mu);
            return gen == g_generation;
        }

        std::string Utf8(const std::wstring& w) {
            if (w.empty()) return {};
            int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
            std::string s(n, '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
            return s;
        }

        std::wstring Wide(const std::string& s) {
            if (s.empty()) return {};
            int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
            std::wstring w(n, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
            return w;
        }

        fs::path Win64Dir() {
            std::wstring buf(32768, L'\0');
            DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
            buf.resize(n);
            return fs::path(buf).parent_path();
        }

        // Same size and same modified time. CopyFileW keeps the modified time, so a copy we made matches.
        bool SameFile(const fs::path& a, const fs::path& b) {
            std::error_code e1, e2, e3, e4;
            auto sa = fs::file_size(a, e1), sb = fs::file_size(b, e2);
            auto ta = fs::last_write_time(a, e3), tb = fs::last_write_time(b, e4);
            return !e1 && !e2 && !e3 && !e4 && sa == sb && ta == tb;
        }

        // "" on success, otherwise a sentence for the player.
        std::string CopyIfChanged(const fs::path& from, const fs::path& to) {
            if (SameFile(from, to)) return "";
            if (CopyFileW(from.c_str(), to.c_str(), FALSE)) return "";
            return "Could not copy " + Utf8(from.filename().wstring()) + " (Windows error " +
                   std::to_string(GetLastError()) + ").";
        }

        int CountProcesses(const wchar_t* exeName) {
            int n = 0;
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snap == INVALID_HANDLE_VALUE) return 0;
            PROCESSENTRY32W pe{ sizeof pe };
            for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
                if (_wcsicmp(pe.szExeFile, exeName) == 0) n++;
            CloseHandle(snap);
            return n;
        }

        DWORD JobProcessCount(HANDLE job) {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
            if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &info, sizeof info, nullptr))
                return 0;
            return info.ActiveProcesses;
        }

        // Closes our previous server, if any, and waits for it to go away.
        void StopPreviousServer() {
            HANDLE job;
            {
                std::lock_guard lock(g_mu);
                job = g_job;
                g_job = nullptr;
            }
            if (!job) return;
            TerminateJobObject(job, 0);
            for (int i = 0; i < 50 && JobProcessCount(job) > 0; i++) Sleep(100);
            CloseHandle(job);
        }

        // The loader reads ColdClientLoader.ini next to itself, and the Steam emulator reads
        // steam_settings next to its dll, so the server gets a folder of its own with both.
        std::string PrepareFiles(const fs::path& win64, const fs::path& idDir, const std::wstring& commandLine) {
            fs::path game = win64 / L"Battleborn.exe", server = win64 / L"Serverborn.exe";
            if (!fs::exists(game)) return "Battleborn.exe is not next to this game. Is the mod installed in Binaries\\Win64?";
            if (std::string err = CopyIfChanged(game, server); !err.empty()) return err;

            std::error_code ec;
            fs::create_directories(idDir / L"steam_settings", ec);
            fs::create_directories(idDir / L"saves", ec);
            for (const wchar_t* f : kLoaderFiles) {
                if (!fs::exists(win64 / f))
                    return Utf8(f) + " is missing from the game folder. Antivirus often removes it: allow it in Windows Security.";
                if (std::string err = CopyIfChanged(win64 / f, idDir / f); !err.empty()) return err;
            }
            if (fs::is_directory(win64 / L"steam_settings"))
                fs::copy(win64 / L"steam_settings", idDir / L"steam_settings",
                         fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);

            std::ofstream ini(idDir / L"ColdClientLoader.ini", std::ios::binary | std::ios::trunc);
            ini << "[SteamClient]\r\n"
                << "Exe=" << Utf8(server.wstring()) << "\r\n"
                << "ExeRunDir=" << Utf8(win64.wstring()) << "\r\n"
                << "ExeCommandLine=" << Utf8(commandLine) << "\r\n"
                << "AppId=394230\r\n"
                << "SteamClientDll=" << Utf8((idDir / L"steamclient.dll").wstring()) << "\r\n"
                << "SteamClient64Dll=" << Utf8((idDir / L"steamclient64.dll").wstring()) << "\r\n"
                << "[Injection]\r\nForceInjectSteamClient=0\r\nForceInjectGameOverlayRenderer=0\r\n"
                << "DllsToInjectFolder=\r\nIgnoreInjectionError=1\r\nIgnoreLoaderArchDifference=0\r\n"
                << "[Persistence]\r\nMode=0\r\n[Debug]\r\nResumeByDebugger=0\r\n";
            if (!ini) return "Could not write the server's ColdClientLoader.ini.";
            ini.close();

            std::ofstream user(idDir / L"steam_settings" / L"configs.user.ini", std::ios::binary | std::ios::trunc);
            user << "[user::general]\r\naccount_name=" << Utf8(kInstance) << "\r\naccount_steamid=" << kSteamId
                 << "\r\nlanguage=english\r\n[user::saves]\r\nlocal_save_path=" << Utf8((idDir / L"saves").wstring()) << "\r\n";
            if (!user) return "Could not write the server's Steam settings.";
            return "";
        }

        void Run(unsigned gen, std::string mapUrl, std::string displayName, int players) {
            StopPreviousServer();
            if (!Current(gen)) return;
            if (CountProcesses(L"Serverborn.exe") > 0) {
                Set(gen, Status::Failed, "Another server is already running on this PC. Close its window, then try again.");
                return;
            }

            fs::path win64 = Win64Dir(), idDir = win64 / L"rb_ids" / kInstance;
            std::wstring readyEvent = L"RebornHostReady_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(gen);
            std::wstring commandLine = L"-windowed -nosound -nomoviestartup -NOSPLASH -nullrhi -rbservermap=" + Wide(mapUrl) +
                L" -rbplayers=" + std::to_wstring(players) + L" -rbinstance=" + kInstance + L" -rbreadyevent=" + readyEvent;

            Set(gen, Status::Starting, "Preparing the server files...");
            if (std::string err = PrepareFiles(win64, idDir, commandLine); !err.empty()) {
                Set(gen, Status::Failed, err);
                return;
            }

            HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, readyEvent.c_str());
            HANDLE job = CreateJobObjectW(nullptr, nullptr);
            if (job) {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
                limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
            }

            fs::path loader = idDir / kLoaderFiles[0];
            std::wstring cmd = L"\"" + loader.wstring() + L"\"";
            STARTUPINFOW si{ sizeof si };
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_SHOWMINNOACTIVE;
            PROCESS_INFORMATION pi{};
            if (!ready || !CreateProcessW(loader.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
                                          nullptr, idDir.c_str(), &si, &pi)) {
                Set(gen, Status::Failed, "Could not start the Steam loader for the server (Windows error " + std::to_string(GetLastError()) + ").");
                if (ready) CloseHandle(ready);
                if (job) CloseHandle(job);
                return;
            }
            // Children of the loader (the server itself) join the job too. If this game is already in a
            // job that forbids it, the server simply is not tied to this game's lifetime.
            if (job && !AssignProcessToJobObject(job, pi.hProcess)) {
                std::printf("[HOST] could not tie the server to this game (error %lu); it will keep running if the game closes\n", GetLastError());
                CloseHandle(job);
                job = nullptr;
            }
            ResumeThread(pi.hThread);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            {
                std::lock_guard lock(g_mu);
                if (gen == g_generation) g_job = job;
                else if (job) { TerminateJobObject(job, 0); CloseHandle(job); job = nullptr; }
            }
            if (!Current(gen)) { CloseHandle(ready); return; }

            Set(gen, Status::Starting, "Starting the server for " + displayName + "... This takes a minute or two.");
            ULONGLONG start = GetTickCount64();
            for (;;) {
                if (WaitForSingleObject(ready, 1000) == WAIT_OBJECT_0) {
                    Set(gen, Status::ReadyToJoin, "Server is ready. Joining...");
                    break;
                }
                if (!Current(gen)) break;
                bool gone = job ? JobProcessCount(job) == 0
                                : (GetTickCount64() - start > 60000 && CountProcesses(L"Serverborn.exe") == 0);
                if (gone) {
                    Set(gen, Status::Failed, "The server closed while starting. Its log is in Documents\\RebornLogs, in the newest host-server folder.");
                    break;
                }
                if (GetTickCount64() - start > kReadyTimeoutMs) {
                    Set(gen, Status::Failed, "The server did not get ready within 5 minutes. Its log is in Documents\\RebornLogs.");
                    break;
                }
            }
            CloseHandle(ready);
        }
    }

    void Start(const std::string& mapUrl, const std::string& displayName, int players) {
        unsigned gen;
        {
            std::lock_guard lock(g_mu);
            gen = ++g_generation;
            g_status = Status::Starting;
            g_message = "Getting ready...";
        }
        std::printf("[HOST] hosting %s (%s) for %d player(s)\n", displayName.c_str(), mapUrl.c_str(), players);
        std::thread(Run, gen, mapUrl, displayName, players).detach();
    }

    void Stop() {
        {
            std::lock_guard lock(g_mu);
            ++g_generation;
            g_status = Status::Idle;
            g_message.clear();
        }
        std::thread(StopPreviousServer).detach();
    }

    Status GetStatus() {
        std::lock_guard lock(g_mu);
        return g_status;
    }

    std::string GetStatusText() {
        std::lock_guard lock(g_mu);
        return g_message;
    }

    bool TakeReadyToJoin() {
        std::lock_guard lock(g_mu);
        if (g_status != Status::ReadyToJoin) return false;
        g_status = Status::Running;
        return true;
    }

    void Dismiss() {
        std::lock_guard lock(g_mu);
        if (g_status == Status::Failed) {
            g_status = Status::Idle;
            g_message.clear();
        }
    }
}

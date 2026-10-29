#include "Perf.hpp"
#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace Perf {
    namespace {
        struct Stat { double sum = 0, max = 0; int n = 0; void Add(double v) { sum += v; max = (std::max)(max, v); n++; } };

        double g_window = 0;   // seconds of frames in this report window
        int g_frames = 0;
        Stat g_frame, g_engine, g_mod, g_net;
        std::map<const char*, Stat> g_parts;
        long long g_considered = 0, g_replicated = 0, g_saturated = 0, g_opened = 0;
        int g_longFrames = 0;  // frames over 50 ms (a visible hitch)

        double Ms(double s) { return s * 1000.0; }
    }

    double Now() {
        static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return (double)t.QuadPart / (double)freq.QuadPart;
    }

    Part::~Part() { g_parts[name].Add(Now() - start); }

    void Frame(float dt, double engineSeconds, double modSeconds) {
        g_window += dt;
        g_frames++;
        g_frame.Add(dt);
        g_engine.Add(engineSeconds);
        g_mod.Add(modSeconds);
        if (dt > 0.05f) g_longFrames++;
    }

    void NetPass(double seconds, int considered, int replicated, int saturatedSkips, int opened) {
        g_net.Add(seconds);
        g_considered += considered;
        g_replicated += replicated;
        g_saturated += saturatedSkips;
        g_opened += opened;
    }

    void Report(bool server) {
        if (g_window < 5.0 || g_frames == 0) return;
        std::vector<std::pair<const char*, Stat>> parts(g_parts.begin(), g_parts.end());
        std::sort(parts.begin(), parts.end(), [](auto& a, auto& b) { return a.second.max > b.second.max; });
        std::string slow;
        for (size_t i = 0; i < parts.size() && i < 4; i++) {
            char b[96];
            snprintf(b, sizeof b, "%s%s %.1f/%.1f", i ? ", " : "", parts[i].first, Ms(parts[i].second.sum / g_frames), Ms(parts[i].second.max));
            slow += b;
        }
        char line[700];
        int n = snprintf(line, sizeof line, "[PERF] %s: %.0f fps, frame avg %.1f max %.1f ms (%d over 50 ms), engine avg %.1f max %.1f ms, mod avg %.2f max %.1f ms; mod parts avg/max ms: %s",
            server ? "server" : "client", g_frames / g_window, Ms(g_frame.sum / g_frame.n), Ms(g_frame.max), g_longFrames,
            Ms(g_engine.sum / g_engine.n), Ms(g_engine.max), Ms(g_mod.sum / g_mod.n), Ms(g_mod.max), slow.empty() ? "none" : slow.c_str());
        if (server && g_net.n)
            snprintf(line + n, sizeof line - n, " | replication %d passes, avg %.2f max %.1f ms, per pass %.0f actors considered, %.0f sends; %lld skipped (connection saturated), %lld channels opened",
                g_net.n, Ms(g_net.sum / g_net.n), Ms(g_net.max), (double)g_considered / g_net.n, (double)g_replicated / g_net.n, g_saturated, g_opened);
        printf("%s\n", line);

        g_window = 0;
        g_frames = 0;
        g_longFrames = 0;
        g_frame = g_engine = g_mod = g_net = Stat();
        g_parts.clear();
        g_considered = g_replicated = g_saturated = g_opened = 0;
    }
}

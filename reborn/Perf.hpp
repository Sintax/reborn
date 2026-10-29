#pragma once
#include <cstdint>

// Frame timing for finding lag. Every 5 s, while frames are being timed, prints
//   [PERF] <server|client>: <fps> fps, frame avg/max ms, engine avg/max ms, mod avg/max ms,
//          slowest mod parts (max ms), and on the server the replication loop's numbers.
namespace Perf {
    // A high-resolution timestamp, in seconds.
    double Now();

    // Once per engine frame: the frame's delta time and how long the engine's own tick and the
    // mod's work after it took (seconds).
    void Frame(float dt, double engineSeconds, double modSeconds);

    // Times one named part of the mod's per-frame work (name must be a string literal).
    struct Part {
        const char* name;
        double start;
        explicit Part(const char* n) : name(n), start(Now()) {}
        ~Part();
    };

    // Server: one replication pass (ServerNetworking::TickNetServer): how long it took, actors
    // considered, channel replications run, actors skipped because a connection was saturated,
    // and channels opened.
    void NetPass(double seconds, int considered, int replicated, int saturatedSkips, int opened);

    // Prints the [PERF] line when 5 s have gone by (call once per frame, after Frame).
    void Report(bool server);
}

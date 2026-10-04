#pragma once
#include <functional>
#include <string>

namespace DebugServer {
    using StateFn = std::function<std::string()>;
    using ExecFn = std::function<std::string(const std::string&)>;

    bool Start(int port, StateFn state, ExecFn exec);
    void Pump();   // game thread, once per tick
    void Stop();
}

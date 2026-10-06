#pragma once
#include <functional>
#include <string>

namespace DebugServer {
    using StateFn = std::function<std::string()>;
    using ExecFn = std::function<std::string(const std::string&)>;
    using BodyFn = std::function<std::string(const std::string& body)>;   // JSON in, JSON out

    struct Routes {
        StateFn state;   // GET /state
        ExecFn exec;     // POST /exec (raw console command)
        BodyFn combat;   // GET /combat
        BodyFn order;    // POST /order
        BodyFn act;      // POST /act
    };

    bool Start(int port, const Routes& routes);
    void Pump();   // game thread, once per tick
    void Stop();
}

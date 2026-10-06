#include "../httplib.h"
#include "../DebugServer.hpp"
#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <thread>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

int main() {
    std::atomic<bool> pumping{ true }, quit{ false };
    CHECK(DebugServer::Start(18999, DebugServer::Routes{
        [] { return std::string(R"({"ok":true})"); },
        [](const std::string& c) { return "ran " + c; },
        [](const std::string&) { return std::string(R"({"enemies":[]})"); },
        [](const std::string& b) { return b == "bad" ? std::string(R"({"error":"unknown_mode","status":400})")
                                                     : "order " + b; },
        [](const std::string&) { return std::string(R"({"error":"no_pawn","status":409})"); } }));
    std::thread game([&] {
        while (!quit) { if (pumping) DebugServer::Pump(); Sleep(10); }
    });

    httplib::Client cli("127.0.0.1", 18999);
    cli.set_read_timeout(10, 0);
    auto r = cli.Get("/ping");
    CHECK(r && r->body == "pong");
    r = cli.Get("/state");
    CHECK(r && r->status == 200 && r->body == R"({"ok":true})");
    r = cli.Post("/exec", "stat fps", "text/plain");
    CHECK(r && r->body == "ran stat fps");
    r = cli.Get("/combat");
    CHECK(r && r->status == 200 && r->body == R"({"enemies":[]})");
    r = cli.Post("/order", "hold", "application/json");
    CHECK(r && r->status == 200 && r->body == "order hold");
    // An {"error":...,"status":N} answer sets the HTTP status, so callers see 400/409, not 200.
    r = cli.Post("/order", "bad", "application/json");
    CHECK(r && r->status == 400 && r->body.find("unknown_mode") != std::string::npos);
    r = cli.Post("/act", R"({"action":"jump"})", "application/json");
    CHECK(r && r->status == 409 && r->body.find("no_pawn") != std::string::npos);

    pumping = false;
    ULONGLONG t0 = GetTickCount64();
    r = cli.Get("/state");
    ULONGLONG dt = GetTickCount64() - t0;
    CHECK(r && r->status == 503);
    CHECK(r && r->body.find("game_thread_unresponsive") != std::string::npos);
    CHECK(dt < 5000);

    quit = true;
    game.join();
    DebugServer::Stop();
    std::puts(failures ? "FAIL" : "PASS");
    return failures;
}

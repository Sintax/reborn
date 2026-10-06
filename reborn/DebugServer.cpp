#include "httplib.h"   // must come before anything that includes Windows.h
#include "DebugServer.hpp"
#include "Diagnostics.hpp"
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

namespace DebugServer {
    namespace {
        std::unique_ptr<httplib::Server> g_server;
        std::thread g_thread;
        std::mutex g_mutex;
        std::queue<std::packaged_task<std::string()>> g_queue;
        Routes g_routes;
        constexpr auto kWait = std::chrono::seconds(3);

        // Game objects may only be touched on the game thread: queue the work and wait for Pump().
        bool RunOnGameThread(std::function<std::string()> fn, std::string& out) {
            std::packaged_task<std::string()> task(std::move(fn));
            auto result = task.get_future();
            {
                std::lock_guard lk(g_mutex);
                g_queue.push(std::move(task));
            }
            if (result.wait_for(kWait) != std::future_status::ready) return false;
            out = result.get();
            return true;
        }

        void Unresponsive(httplib::Response& res) {
            res.status = 503;
            res.set_content(R"({"error":"game_thread_unresponsive","ms_since_tick":)" +
                            std::to_string(Diagnostics::MillisSinceLastTick()) + "}", "application/json");
        }

        void Answer(httplib::Response& res, std::function<std::string()> fn, const char* type) {
            std::string out;
            try {
                if (!RunOnGameThread(std::move(fn), out)) return Unresponsive(res);
            }
            catch (const std::exception& e) {
                res.status = 500;
                res.set_content(e.what(), "text/plain");
                return;
            }
            res.set_content(out, type);
        }

        // Like Answer, but a JSON body of the form {"error":"...","status":NNN} sets the HTTP status.
        void AnswerJson(httplib::Response& res, std::function<std::string()> fn) {
            std::string out;
            try {
                if (!RunOnGameThread(std::move(fn), out)) return Unresponsive(res);
            }
            catch (const std::exception& e) {
                res.status = 500;
                res.set_content(std::string("{\"error\":\"") + e.what() + "\"}", "application/json");
                return;
            }
            if (out.rfind("{\"error\"", 0) == 0) {
                size_t at = out.find("\"status\":");
                res.status = at == std::string::npos ? 400 : std::atoi(out.c_str() + at + 9);
            }
            res.set_content(out, "application/json");
        }
    }

    bool Start(int port, const Routes& routes) {
        g_routes = routes;
        g_server = std::make_unique<httplib::Server>();
        g_server->Get("/ping", [](const httplib::Request&, httplib::Response& res) {
            res.set_content("pong", "text/plain");
        });
        g_server->Get("/state", [](const httplib::Request&, httplib::Response& res) {
            Answer(res, g_routes.state, "application/json");
        });
        g_server->Post("/exec", [](const httplib::Request& req, httplib::Response& res) {
            std::string cmd = req.body;
            Answer(res, [cmd] { return g_routes.exec(cmd); }, "text/plain");
        });
        g_server->Get("/combat", [](const httplib::Request&, httplib::Response& res) {
            AnswerJson(res, [] { return g_routes.combat(""); });
        });
        g_server->Post("/order", [](const httplib::Request& req, httplib::Response& res) {
            std::string body = req.body;
            AnswerJson(res, [body] { return g_routes.order(body); });
        });
        g_server->Post("/act", [](const httplib::Request& req, httplib::Response& res) {
            std::string body = req.body;
            AnswerJson(res, [body] { return g_routes.act(body); });
        });
        g_server->Get("/log", [](const httplib::Request&, httplib::Response& res) {
            std::string s;
            for (auto& line : Diagnostics::RecentLines()) { s += line; s += '\n'; }
            res.set_content(s, "text/plain");
        });
        if (!g_server->bind_to_port("127.0.0.1", port)) {
            std::printf("[DEBUG] could not bind 127.0.0.1:%d\n", port);
            return false;
        }
        g_thread = std::thread([] { g_server->listen_after_bind(); });
        std::printf("[DEBUG] debug server on 127.0.0.1:%d\n", port);
        return true;
    }

    void Pump() {
        for (int i = 0; i < 8; i++) {
            std::packaged_task<std::string()> task;
            {
                std::lock_guard lk(g_mutex);
                if (g_queue.empty()) return;
                task = std::move(g_queue.front());
                g_queue.pop();
            }
            task();
        }
    }

    void Stop() {
        if (g_server) g_server->stop();
        if (g_thread.joinable()) g_thread.join();
        g_server.reset();
    }
}

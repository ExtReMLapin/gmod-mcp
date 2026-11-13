// Multi-instance coordination.
//
// The first instance that binds the base port is the coordinator. Every other instance (member) sends a heartbeat to
// the base port every 2 s (registration is an upsert, authenticated with the shared bearer token). When the
// coordinator stops answering, members try to bind the base port: the exclusive bind guarantees a single winner,
// and the others simply re-register with it on their next heartbeat.
#include "coordinator.h"

#ifdef _WIN32
#include <winsock2.h>
#endif
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>

#include "agent.h"
#include "console.h"
#include "http_server.h"
#include "jobs.h"
#include "log.h"
#include "mcp.h"
#include "platform/platform.h"
#include "realms.h"

using nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace coordinator {

static const double kHeartbeatS = 2.0;
static const double kDeadAfterS = 10.0;
static const int kFailuresBeforeElection = 3;

struct Member {
    json info;  // id, mode, port, pid, map, realms
    Clock::time_point last_seen;
};

static std::mutex g_mu;
static std::map<std::string, Member> g_members;
static std::atomic<bool> g_run{false};
static std::thread g_thread;
static std::atomic<int> g_failures{0};

bool is_coordinator() {
    int base = agent::config().base_port;
    return httpsrv::port() == base || (httpsrv::has_extra() && httpsrv::extra_port() == base);
}

static json self_info() {
    const auto &c = agent::config();
    json realms_j = json::object();
    for (int r = 0; r < realms::COUNT; ++r) realms_j[realms::name(r)] = agent::realm_present(r);
    // Health at a glance (sent with every heartbeat): agents pick a usable instance without one status call each.
    json fps = jobs::frame_stats()["fps"];
    double since = jobs::seconds_since_last_frame();
    return json{{"id", c.instance_id}, {"mode", agent::mode_name()}, {"port", c.port}, {"pid", plat::process_id()},
                {"map", agent::current_map()}, {"realms", realms_j}, {"agent_version", c.version},
                {"uptime_s", (int64_t)console::uptime()}, {"fps", fps.is_number() ? json((int)(fps.get<double>() + 0.5)) : json(nullptr)},
                {"main_thread_stalled", since > 5.0}, {"lua_errors", console::errors_cursor()}};
}

static void purge_locked() {
    auto now = Clock::now();
    for (auto it = g_members.begin(); it != g_members.end();) {
        double age = std::chrono::duration<double>(now - it->second.last_seen).count();
        if (age > kDeadAfterS) {
            logx::info("instance %s removed (no heartbeat for %.0f s)", it->first.c_str(), age);
            it = g_members.erase(it);
        } else {
            ++it;
        }
    }
}

json instances() {
    json arr = json::array();
    json me = self_info();
    me["self"] = true;
    me["role"] = is_coordinator() ? "coordinator" : "member";
    me["heartbeat_age_s"] = 0;
    arr.push_back(me);
    std::lock_guard<std::mutex> lk(g_mu);
    purge_locked();
    auto now = Clock::now();
    for (auto &kv : g_members) {
        json e = kv.second.info;
        e["self"] = false;
        e["role"] = "member";
        e["heartbeat_age_s"] = std::chrono::duration<double>(now - kv.second.last_seen).count();
        arr.push_back(e);
    }
    return arr;
}

static std::unique_ptr<httplib::Client> client(int port, double read_timeout_s) {
    auto cli = std::make_unique<httplib::Client>("127.0.0.1", port);
    cli->set_connection_timeout(1, 0);
    cli->set_read_timeout((time_t)read_timeout_s, 0);
    cli->set_write_timeout(10, 0);
    cli->set_bearer_token_auth(agent::config().token);
    return cli;
}

// Coordinator side: heartbeat / registration (upsert).
static json on_heartbeat(const json &in) {
    if (!is_coordinator()) return json{{"ok", false}, {"error", "not_coordinator"}};
    if (!in.contains("id") || !in.contains("port")) return json{{"ok", false}, {"error", "bad_request"}};
    std::string id = in["id"];
    if (id == agent::config().instance_id) return json{{"ok", true}};
    std::lock_guard<std::mutex> lk(g_mu);
    bool fresh = g_members.find(id) == g_members.end();
    g_members[id] = Member{in, Clock::now()};
    if (fresh) logx::info("instance %s registered (port %d, %s)", id.c_str(), in.value("port", 0), in.value("mode", "?").c_str());
    return json{{"ok", true}, {"coordinator", agent::config().instance_id}};
}

// Relay of a tools/call to another instance (coordinator only).
static json relay(const std::string &instance, const std::string &tool, const json &args) {
    auto wrap_err = [](const std::string &code, const std::string &msg) {
        json e{{"error", {{"code", code}, {"message", msg}}}};
        return json{{"content", json::array({json{{"type", "text"}, {"text", e.dump()}}})}, {"structuredContent", e}, {"isError", true}};
    };
    if (!is_coordinator()) return wrap_err("not_coordinator", "this instance is not the coordinator: call the coordinator (base port)");
    int port = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        purge_locked();
        auto it = g_members.find(instance);
        if (it == g_members.end()) return wrap_err("unknown_instance", "unknown or dead instance: " + instance + " (see instances_list)");
        port = it->second.info.value("port", 0);
    }
    auto cli = client(port, 900);
    json req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", {{"name", tool}, {"arguments", args}}}};
    auto res = cli->Post("/mcp", req.dump(), "application/json");
    if (!res) return wrap_err("instance_unreachable", "instance " + instance + " did not answer (dead?)");
    try {
        json r = json::parse(res->body);
        if (r.contains("result")) {
            json out = r["result"];
            if (out.contains("structuredContent") && out["structuredContent"].is_object()) out["structuredContent"]["relayed_by"] = agent::config().instance_id;
            return out;
        }
        return wrap_err("relay_error", r.value("error", json::object()).value("message", "unknown error"));
    } catch (const std::exception &e) {
        return wrap_err("relay_error", std::string("invalid response: ") + e.what());
    }
}

// Member side: one heartbeat; returns true when the coordinator answered.
static bool heartbeat() {
    auto cli = client(agent::config().base_port, 3);
    auto res = cli->Post("/internal/heartbeat", self_info().dump(), "application/json");
    return res && res->status == 200;
}

static void loop() {
    while (g_run) {
        for (int i = 0; i < (int)(kHeartbeatS * 10) && g_run; ++i) plat::sleep_ms(100);
        if (!g_run) break;
        if (is_coordinator()) {
            std::lock_guard<std::mutex> lk(g_mu);
            purge_locked();
            continue;
        }
        if (heartbeat()) {
            g_failures = 0;
            continue;
        }
        if (++g_failures < kFailuresBeforeElection) continue;
        // Coordinator gone: try to take over the base port.
        if (httpsrv::try_bind_extra(agent::config().base_port)) {
            logx::info("coordinator unreachable: this instance is now the coordinator (port %d)", agent::config().base_port);
            g_failures = 0;
        }
    }
}

void start() {
    httpsrv::add_internal_route("heartbeat", on_heartbeat);
    mcp::set_relay(relay);
    mcp::set_peers([] {
        std::vector<std::string> ids;
        if (!is_coordinator()) return ids;
        std::lock_guard<std::mutex> lk(g_mu);
        purge_locked();
        for (auto &kv : g_members) ids.push_back(kv.first);
        return ids;
    });
    if (!is_coordinator()) {
        if (heartbeat()) logx::info("registered with the coordinator on port %d", agent::config().base_port);
        else logx::warn("coordinator on port %d did not answer (will retry)", agent::config().base_port);
    }
    g_run = true;
    g_thread = std::thread(loop);
}

void stop() {
    g_run = false;
    if (g_thread.joinable()) g_thread.join();
}

}  // namespace coordinator

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

// Members recently removed (no heartbeat): their id, pid and when, so a later call gets a useful answer.
struct Gone {
    uint32_t pid;
    Clock::time_point when;
};
static std::map<std::string, Gone> g_gone;

static void purge_locked() {
    auto now = Clock::now();
    for (auto it = g_members.begin(); it != g_members.end();) {
        double age = std::chrono::duration<double>(now - it->second.last_seen).count();
        if (age > kDeadAfterS) {
            logx::info("instance %s removed (no heartbeat for %.0f s)", it->first.c_str(), age);
            g_gone[it->first] = Gone{it->second.info.value("pid", 0u), it->second.last_seen};
            if (g_gone.size() > 200) g_gone.erase(g_gone.begin());
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

std::string gone_message(const std::string &id, uint32_t pid, double seconds_ago, bool process_alive) {
    std::string who = "instance " + id + (pid ? " (pid " + std::to_string(pid) + ")" : "");
    std::string when = " stopped answering " + std::to_string((int)seconds_ago) + " s ago";
    if (!process_alive)
        return who + when + ": its process has exited (closed, or crashed: see garrysmod/console.log or the crash dialog). "
                            "Relaunch a client with instances_launch.";
    return who + when + " but its process is still running: frozen (infinite loop, long load) or busy. Check it with "
                        "status {instance}; game_terminate {instance} ends it.";
}


// Relay of a tools/call to another instance (coordinator only).
static json relay(const std::string &instance, const std::string &tool, const json &args) {
    auto wrap_err = [](const std::string &code, const std::string &msg) {
        json e{{"error", {{"code", code}, {"message", msg}}}};
        return json{{"content", json::array({json{{"type", "text"}, {"text", e.dump()}}})}, {"structuredContent", e}, {"isError", true}};
    };
    if (!is_coordinator()) return wrap_err("not_coordinator", "this instance is not the coordinator: call the coordinator (base port)");
    int port = 0;
    uint32_t pid = 0;
    Clock::time_point last_seen = Clock::now();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        purge_locked();
        auto it = g_members.find(instance);
        if (it == g_members.end()) {
            auto g = g_gone.find(instance);
            if (g != g_gone.end()) {
                double ago = std::chrono::duration<double>(Clock::now() - g->second.when).count();
                return wrap_err("instance_gone", gone_message(instance, g->second.pid, ago, plat::process_start_stamp(g->second.pid) != 0));
            }
            return wrap_err("unknown_instance", "unknown instance: " + instance + " (see instances_list)");
        }
        port = it->second.info.value("port", 0);
        pid = it->second.info.value("pid", 0u);
        last_seen = it->second.last_seen;
    }
    auto cli = client(port, 900);
    json req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", {{"name", tool}, {"arguments", args}}}};
    auto res = cli->Post("/mcp", req.dump(), "application/json");
    if (!res) {
        bool alive = pid && plat::process_start_stamp(pid) != 0;
        double ago = std::chrono::duration<double>(Clock::now() - last_seen).count();
        return wrap_err(alive ? "instance_unreachable" : "instance_gone", gone_message(instance, pid, ago, alive));
    }
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
// State of this member's link to the coordinator, for status: "registered", "refused" (another token: the coordinator
// answers 401, so this game is not reachable through it) or "unreachable".
static std::mutex g_link_mu;
static std::string g_link = "unknown";
std::string link_state() {
    if (is_coordinator()) return "coordinator";
    std::lock_guard<std::mutex> lk(g_link_mu);
    return g_link;
}
static void set_link(const std::string &state) {
    std::lock_guard<std::mutex> lk(g_link_mu);
    if (state == g_link) return;
    g_link = state;
    if (state == "refused")
        logx::warn("the coordinator on port %d refused this instance (HTTP 401: it uses another token). This game is not "
                   "reachable through it: give every game the same gmod-mcp config folder (APPDATA on Windows, "
                   "XDG_CONFIG_HOME or ~/.config on Linux)", agent::config().base_port);
}

static bool heartbeat() {
    auto cli = client(agent::config().base_port, 3);
    auto res = cli->Post("/internal/heartbeat", self_info().dump(), "application/json");
    if (res && (res->status == 401 || res->status == 403)) set_link("refused");
    else set_link(res && res->status == 200 ? "registered" : "unreachable");
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

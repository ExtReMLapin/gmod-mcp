#include "agent.h"

#include <embedded_lua.h>

#include <chrono>
#include <mutex>

#include "console.h"
#include "events.h"
#include "installer.h"
#include "jobs.h"
#include "log.h"
#include "luaapi.h"
#include "netaddr.h"
#include "platform/platform.h"
#include "realms.h"
#include "util.h"

using nlohmann::json;

namespace agent {

static Config g_cfg;
static lua_State *g_host = nullptr;
static int g_host_realm = realms::MENU;

static std::atomic<uint64_t> g_gen[realms::COUNT];
static std::atomic<bool> g_present[realms::COUNT];
static std::atomic<bool> g_injected[realms::COUNT];
static double g_inject_retry_at[realms::COUNT] = {0, 0, 0};
static std::atomic<uint64_t> g_ready_gen[realms::COUNT];  // generation for which the realm is "ready"
static bool g_was_present[realms::COUNT] = {false, false, false};
static uint64_t g_map_loaded_key = 0;
static bool g_menu_ready_sent = false;
static std::mutex g_map_mu;
static std::string g_map;

std::string current_map() {
    std::lock_guard<std::mutex> lk(g_map_mu);
    return g_map;
}


static std::mutex g_lock_mu;
static json g_lock_status = json{{"client_allowed", false}, {"reason", "no client realm"}};

Config &config() { return g_cfg; }
const char *mode_name() { return g_cfg.mode == Mode::Client ? "client" : "srcds"; }
lua_State *host_state() { return g_host; }
int host_realm() { return g_host_realm; }
uint64_t generation(int r) { return (r >= 0 && r < realms::COUNT) ? g_gen[r].load() : 0; }
bool realm_present(int r) { return (r >= 0 && r < realms::COUNT) && g_present[r].load(); }
bool realm_injected(int r) { return (r >= 0 && r < realms::COUNT) && g_injected[r].load(); }

json err(const std::string &code, const std::string &message, json extra) {
    json e{{"code", code}, {"message", message}};
    if (!extra.is_null()) e["details"] = extra;
    return json{{"ok", false}, {"error", e}};
}

json lock_status() {
    std::lock_guard<std::mutex> lk(g_lock_mu);
    return g_lock_status;
}

// --- Client-side network lock ---
bool client_allowed(std::string *reason) {
    std::string why;
    bool allowed = false;
    json addr_info = nullptr;
    if (g_cfg.mode != Mode::Client) {
        why = "srcds mode: no client realm";
    } else if (realms::state(realms::SERVER) != nullptr) {
        allowed = true;
        why = "the process hosts the server (singleplayer or listen server)";
    } else {
        netaddr::Result a = netaddr::current_server_address();
        addr_info = json{{"address", a.address}, {"method", a.method}};
        if (!a.ok) {
            why = "server address unknown (" + a.error + "): refused for safety";
        } else {
            std::string how;
            if (netaddr::is_this_machine(a.address, &how)) {
                allowed = true;
                why = "server runs on this machine (" + a.address + ", " + how + ")";
            } else {
                why = "connected to a server that is not on this machine (" + a.address + "): client realm locked";
            }
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_lock_mu);
        bool changed = g_lock_status.value("client_allowed", false) != allowed || g_lock_status.value("reason", "") != why;
        g_lock_status = json{{"client_allowed", allowed}, {"reason", why}};
        if (!addr_info.is_null()) g_lock_status["server_address"] = addr_info;
        if (changed && realms::iface(realms::CLIENT) != nullptr)
            (allowed ? logx::info : logx::warn)("client lock: %s (%s)", allowed ? "allowed" : "REFUSED", why.c_str());
    }
    if (reason) *reason = why;
    return allowed;
}

// --- Native functions exposed to the Lua runtime ---
static int native_push_error(lua_State *L) {
    try {
        json e = json::parse(lua::tostring(L, 1));
        console::push_error(e);
    } catch (...) {
    }
    return 0;
}

static int native_frames(lua_State *L) {
    lua::api().pushnumber(L, (double)jobs::frame_count());
    return 1;
}

static int native_console_cursor(lua_State *L) {
    lua::api().pushnumber(L, (double)console::cursor_now());
    return 1;
}

static int native_uptime(lua_State *L) {
    lua::api().pushnumber(L, console::uptime());
    return 1;
}

// native.compile(code, chunkname) -> function | nil, message
static int native_compile(lua_State *L) {
    const auto &A = lua::api();
    size_t n = 0;
    const char *code = A.tolstring(L, 1, &n);
    if (!code) {
        A.pushnil(L);
        A.pushstring(L, "code: string expected");
        return 2;
    }
    std::string chunk = lua::tostring(L, 2);
    if (chunk.empty()) chunk = "=mcpagent";
    if (A.loadbuffer(L, code, n, chunk.c_str()) != 0) {
        A.pushnil(L);
        A.insert(L, -2);
        return 2;
    }
    return 1;
}

// native.event(type, json): publishes an event to the stream.
static int native_event(lua_State *L) {
    std::string type = lua::tostring(L, 1);
    json data;
    try {
        std::string s = lua::tostring(L, 2);
        data = s.empty() ? json::object() : json::parse(s);
    } catch (...) {
        data = json::object();
    }
    events::publish(type, data);
    return 0;
}

// native.setup_info() -> string: ready-to-paste client configuration (contains the token). Exposed to the host
// state only (menu, or server on srcds) and printed only on explicit request (mcpagent_setup console command).
static int native_setup_info(lua_State *L) {
    const auto &c = g_cfg;
    std::string url = "http://127.0.0.1:" + std::to_string(c.base_port) + "/mcp";
    std::string s;
    s += "gmod-mcp is listening on " + url + " (this instance: port " + std::to_string(c.port) + ").\n";
    s += "Claude Code - install the plugin (it stays connected while the game is closed), in Claude Code:\n\n";
    s += "  /plugin install gmod-mcp --marketplace ExtReMLapin/gmod-mcp\n\n";
    s += "  or, without the plugin, in a terminal (then restart Claude Code or use /mcp):\n\n";
    s += "  claude mcp add --transport http gmod " + url + " --header \"Authorization: Bearer " + c.token + "\"\n\n";
    s += "Other MCP clients (Streamable HTTP), e.g. .mcp.json:\n\n";
    s += "  {\"mcpServers\": {\"gmod\": {\"type\": \"http\", \"url\": \"" + url + "\", \"headers\": {\"Authorization\": \"Bearer " +
         c.token + "\"}}}}\n\n";
    s += "Keep the token private: it gives full control of this game.\n";
    lua::api().pushstring(L, s.c_str());
    return 1;
}

// native.uninstall() -> message: removes the main-menu loader from menu.lua (client mode, menu state only).
static int native_uninstall(lua_State *L) {
    std::string e;
    std::string msg = installer::remove_menu_patch(g_cfg.game_dir, e)
                          ? "gmod-mcp: main-menu loader removed from lua/menu/menu.lua. To uninstall completely, also delete "
                            "lua/bin/gmsv_mcpagent_*.dll and lua/autorun/server/mcpagent_boot.lua (otherwise the loader is "
                            "re-installed the next time the game runs with -mcp and a map is loaded)."
                          : "gmod-mcp: could not remove the loader: " + e;
    lua::api().pushstring(L, msg.c_str());
    return 1;
}

// native.engine_cmd(line) -> true | nil, err  (client mode only; exposed to the menu state only)
static int native_engine_cmd(lua_State *L) {
    const auto &A = lua::api();
    std::string e;
    if (g_cfg.mode == Mode::Client && netaddr::client_cmd(lua::tostring(L, 1), e)) {
        A.pushboolean(L, 1);
        return 1;
    }
    A.pushnil(L);
    A.pushstring(L, e.empty() ? "unavailable in srcds mode" : e.c_str());
    return 2;
}

// native.report(channel, json)
static int native_report(lua_State *L) {
    std::string ch = lua::tostring(L, 1);
    std::string realm = lua::tostring(L, 3);
    json v;
    try {
        v = json::parse(lua::tostring(L, 2));
    } catch (...) {
        v = nullptr;
    }
    events::report(ch, realm, v);
    return 0;
}

static bool has_runtime(lua_State *L) {
    const auto &A = lua::api();
    A.getfield(L, lua::GLOBALSINDEX, "__mcpagent");
    bool ok = A.type(L, -1) == lua::TTABLE;
    if (ok) {
        A.getfield(L, -1, "dispatch");
        ok = A.type(L, -1) == lua::TFUNCTION;
        lua::pop(L, 1);
    }
    lua::pop(L, 1);
    return ok;
}

static bool hook_ready(lua_State *L) {
    const auto &A = lua::api();
    A.getfield(L, lua::GLOBALSINDEX, "hook");
    bool ok = A.type(L, -1) == lua::TTABLE;
    lua::pop(L, 1);
    return ok;
}

static bool inject(int realm, lua_State *L, std::string &e) {
    const auto &A = lua::api();
    // Native table, set before the chunks.
    A.createtable(L, 0, 8);
    A.pushcclosure(L, native_push_error, 0);
    A.setfield(L, -2, "push_error");
    A.pushcclosure(L, native_frames, 0);
    A.setfield(L, -2, "frames");
    A.pushcclosure(L, native_console_cursor, 0);
    A.setfield(L, -2, "console_cursor");
    A.pushcclosure(L, native_uptime, 0);
    A.setfield(L, -2, "uptime");
    A.pushcclosure(L, native_compile, 0);
    A.setfield(L, -2, "compile");
    A.pushcclosure(L, native_event, 0);
    A.setfield(L, -2, "event");
    A.pushcclosure(L, native_report, 0);
    A.setfield(L, -2, "report");
    if (realm == (g_cfg.mode == Mode::Client ? realms::MENU : realms::SERVER)) {
        A.pushcclosure(L, native_setup_info, 0);
        A.setfield(L, -2, "setup_info");
        if (g_cfg.mode == Mode::Client) {
            A.pushcclosure(L, native_uninstall, 0);
            A.setfield(L, -2, "uninstall");
        }
    }
    if (realm == realms::MENU) {
        A.pushcclosure(L, native_engine_cmd, 0);
        A.setfield(L, -2, "engine_cmd");
    }
    A.pushstring(L, realms::name(realm));
    A.setfield(L, -2, "realm");
    A.pushstring(L, mode_name());
    A.setfield(L, -2, "mode");
    A.pushstring(L, g_cfg.version.c_str());
    A.setfield(L, -2, "version");
    A.pushnumber(L, (double)(g_gen[realm].load() + 1));
    A.setfield(L, -2, "generation");
    A.setfield(L, lua::GLOBALSINDEX, "__mcpagent_native");
    for (size_t i = 0; i < embedded::lua_chunk_count; ++i) {
        const auto &c = embedded::lua_chunks[i];
        std::string code(reinterpret_cast<const char *>(c.data), c.size);
        std::string chunk = std::string("@mcpagent/") + c.name;
        if (!lua::run(L, code, chunk.c_str(), nullptr, e)) {
            e = std::string(c.name) + ": " + e;
            lua::run(L, "__mcpagent = nil", "=mcpagent", nullptr, e);
            return false;
        }
    }
    return true;
}

static void ensure_runtime(int realm, lua_State *L) {
    if (has_runtime(L)) {
        g_injected[realm] = true;
        return;
    }
    g_injected[realm] = false;
    if (realm == realms::CLIENT && !client_allowed()) return;
    if (!hook_ready(L)) return;
    double now = console::uptime();
    if (now < g_inject_retry_at[realm]) return;
    std::string e;
    if (inject(realm, L, e)) {
        g_gen[realm]++;
        g_injected[realm] = true;
        logx::info("runtime injected into realm %s (generation %llu)", realms::name(realm),
                   (unsigned long long)g_gen[realm].load());
        events::publish("runtime_injected", json{{"realm", realms::name(realm)}, {"generation", g_gen[realm].load()}});
        if (realm == realms::MENU && !g_menu_ready_sent) {
            g_menu_ready_sent = true;
            events::publish("menu_ready", json{{"instance", g_cfg.instance_id}, {"port", g_cfg.port}});
        }
    } else {
        g_inject_retry_at[realm] = now + 5.0;
        static std::string last_err[realms::COUNT];
        if (last_err[realm] != e) {
            last_err[realm] = e;
            logx::warn("failed to inject the runtime into %s: %s", realms::name(realm), e.c_str());
            events::publish("runtime_error", json{{"realm", realms::name(realm)}, {"message", e}});
        }
    }
}

bool realm_ready(int r) {
    return (r >= 0 && r < realms::COUNT) && g_present[r].load() && g_ready_gen[r].load() == g_gen[r].load() && g_gen[r].load() > 0;
}

// One-time in-game notice after the main-menu loader was installed or updated: shown to the local player once the
// client realm is really in game (the runtime adds its own delay).
static std::string g_pending_notice;

void notify_player(const std::string &text) { g_pending_notice = text; }

static void deliver_notice() {
    if (g_pending_notice.empty() || !realm_ready(realms::CLIENT) || !client_allowed()) return;
    json r = call_runtime(realms::CLIENT, "notify", json{{"text", g_pending_notice}, {"delay", 1}});
    if (r.value("ok", false)) g_pending_notice.clear();
}

// Asks the runtime whether the realm is ready (gamemode loaded, local player valid...).
static void poll_ready(int r) {
    if (realm_ready(r) || !g_injected[r]) return;
    json res = call_runtime(r, "ready", json::object());
    if (res.value("ok", false) && res["result"].value("ready", false)) {
        g_ready_gen[r] = g_gen[r].load();
        events::publish("realm_ready", json{{"realm", realms::name(r)}, {"generation", g_gen[r].load()},
                                            {"map", res["result"].value("map", "")}});
    }
}

static void check_map_loaded() {
    bool sv = realm_ready(realms::SERVER);
    bool cl = g_cfg.mode == Mode::Dedicated || realm_ready(realms::CLIENT);
    static uint64_t settle_key = 0, settle_since = 0;
    if (!sv || !cl) {
        settle_key = 0;
        return;
    }
    uint64_t key = g_gen[realms::SERVER].load() * 1000003ULL + (g_cfg.mode == Mode::Dedicated ? 0 : g_gen[realms::CLIENT].load());
    if (key == g_map_loaded_key) return;
    // Client mode: wait for the menu loading screen to be torn down, then 15 settling frames
    // (chaining a "map" during teardown crashed vgui/menusystem).
    if (g_cfg.mode == Mode::Client) {
        json idle = call_runtime(realms::MENU, "menu_idle", json::object());
        if (!idle.value("ok", false) || idle["result"].value("loading", true)) {
            settle_key = 0;
            return;
        }
    }
    uint64_t fc = jobs::frame_count();
    if (settle_key != key) {
        settle_key = key;
        settle_since = fc;
        return;
    }
    if (fc - settle_since < 15) return;
    g_map_loaded_key = key;
    json r = call_runtime(realms::SERVER, "status", json::object());
    json st = r.value("ok", false) ? r["result"] : json::object();
    {
        std::lock_guard<std::mutex> lk(g_map_mu);
        g_map = st.value("map", "");
    }
    events::publish("map_loaded", json{{"map", st.value("map", "")},
                                       {"gamemode", st.value("gamemode", "")},
                                       {"maxplayers", st.value("maxplayers", 0)},
                                       {"server_generation", g_gen[realms::SERVER].load()},
                                       {"client_generation", g_gen[realms::CLIENT].load()}});
}

static std::atomic<bool> g_jobs_active[realms::COUNT];

void jobs_wake(int realm) {
    if (realm >= 0 && realm < realms::COUNT) g_jobs_active[realm] = true;
}

// Resumes the lua_job coroutines of every realm that has some (once per frame).
static void tick_lua_jobs() {
    for (int r = 0; r < realms::COUNT; ++r) {
        if (!g_jobs_active[r]) continue;
        if (!realms::state(r)) {
            g_jobs_active[r] = false;  // the state is gone, and its jobs with it
            continue;
        }
        json t = call_runtime(r, "job_tick", json::object());
        if (!t.value("ok", false) || t["result"].value("active", 0) == 0) g_jobs_active[r] = false;
    }
}

// A game started in the background (for example with +map from a script) can believe it has the focus while another
// application's window is in front: it then captures the mouse and keeps pulling the cursor to the center of its window.
// When that state lasts, send the game what Windows sends on a normal focus loss. Gives up after a few tries until the
// game is in front again, so a build that ignores the message is not spammed.
static void check_focus() {
    static int stale = 0, tries = 0;
    if (plat::process_has_foreground() != 0) {
        stale = tries = 0;
        return;
    }
    lua_State *menu = realms::state(realms::MENU);
    std::string out, e;
    if (!menu || !lua::run(menu, "return tostring(system and system.HasFocus and system.HasFocus())", "=mcpagent:focus", &out, e) ||
        out != "true") {
        stale = 0;
        return;
    }
    if (++stale < 2 || tries >= 3) return;
    stale = 0;
    ++tries;
    if (plat::send_focus_lost()) {
        logx::info("the game believed it had the focus while another window is in front: told it the focus was lost "
                   "(releases the mouse)");
        events::publish("focus_released", json{{"try", tries}});
    }
}

void pump(lua_State *host) {
    // Re-entrancy guard: user code run by a job may call __mcpagent_pump() itself.
    static bool in_pump = false;
    if (in_pump) return;
    struct Guard {
        Guard() { in_pump = true; }
        ~Guard() { in_pump = false; }
    } guard;
    g_host = host;
    jobs::on_frame();
    uint64_t fc = jobs::frame_count();
    if (fc % 30 == 0) console::maintain();
    for (int r = 0; r < realms::COUNT; ++r) {
        lua_State *L = realms::state(r);
        bool present = L != nullptr;
        g_present[r] = present;
        if (present != g_was_present[r]) {
            g_was_present[r] = present;
            events::publish(present ? "realm_up" : "realm_down", json{{"realm", realms::name(r)}});
            if (!present && r != realms::MENU && g_cfg.mode == Mode::Client && !realms::state(realms::CLIENT) &&
                !realms::state(realms::SERVER))
            {
                {
                    std::lock_guard<std::mutex> lk(g_map_mu);
                    g_map.clear();
                }
                events::publish("disconnected", json::object());
            }
        }
        if (!L) {
            g_injected[r] = false;
            continue;
        }
        if (g_cfg.mode == Mode::Dedicated && r != realms::SERVER) continue;
        ensure_runtime(r, L);
        if (r != realms::MENU && fc % 10 == 0) poll_ready(r);
    }
    if (fc % 10 == 0) check_map_loaded();
    if (fc % 30 == 0) deliver_notice();
    tick_lua_jobs();
    // Refreshes the lock state (for status) even without a client call.
    if (g_cfg.mode == Mode::Client && jobs::frame_count() % 15 == 0) {
        client_allowed();
        check_focus();
    }
    jobs::pump(50.0);
}

json call_runtime(int realm, const std::string &tool, const json &args) {
    if (realm < 0 || realm >= realms::COUNT) return err("invalid_realm", "unknown realm");
    if (g_cfg.mode == Mode::Dedicated && realm != realms::SERVER)
        return err("realm_unavailable", std::string("realm ") + realms::name(realm) + " unavailable in srcds mode");
    lua_State *L = realms::state(realm);
    if (!L) return err("realm_unavailable", std::string("realm ") + realms::name(realm) + " unavailable (not loaded)");
    if (realm == realms::CLIENT) {
        std::string why;
        if (!client_allowed(&why)) return err("client_locked", why);
    }
    ensure_runtime(realm, L);
    if (!has_runtime(L))
        return err("runtime_unavailable", std::string("runtime not injected into ") + realms::name(realm) + " (state still initializing?)");
    std::string out, e;
    json payload{{"tool", tool}, {"args", args}};
    if (!lua::call_string(L, "__mcpagent", "dispatch", payload.dump(-1, ' ', false, json::error_handler_t::replace), out, e))
        return err("lua_dispatch_failed", e);
    json r;
    try {
        r = json::parse(out);
    } catch (const std::exception &ex) {
        return err("bad_runtime_output", std::string("invalid JSON output from the runtime: ") + ex.what());
    }
    // Optional binary data (screenshots): read outside the JSON.
    if (r.is_object() && r.value("blob", false)) {
        const auto &A = lua::api();
        A.getfield(L, lua::GLOBALSINDEX, "__mcpagent");
        A.getfield(L, -1, "blob");
        std::string blob = lua::tostring(L, -1);
        lua::pop(L, 1);
        A.pushnil(L);
        A.setfield(L, -2, "blob");
        lua::pop(L, 1);
        r["blob_b64"] = util::base64_encode(blob);
    }
    r["realm"] = realms::name(realm);
    return r;
}

json rt(int realm, const std::string &tool, const json &args, double timeout_s) {
    json r = jobs::run_on_main([=] { return call_runtime(realm, tool, args); }, timeout_s);
    if (jobs::is_timeout(r))
        return err("main_thread_timeout",
                   "the main thread did not respond in time (map loading, freeze or infinite loop?)",
                   json{{"timeout_s", timeout_s}, {"job_started", r.value("started", false)},
                        {"seconds_since_last_frame", jobs::seconds_since_last_frame()}});
    if (r.is_object() && r.contains("__error")) return err("internal", r["__error"].get<std::string>());
    return r;
}

}  // namespace agent

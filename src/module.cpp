// Binary module entry point: gmod13_open / gmod13_close.
#include <string>

#include "agent.h"
#include "console.h"
#include "coordinator.h"
#include "crashguard.h"
#include "events.h"
#include "http_server.h"
#include "installer.h"
#include "lua_mount.h"
#include "jobs.h"
#include "log.h"
#include "luaapi.h"
#include "mcp.h"
#include "platform/platform.h"
#include "realms.h"
#include "resources.h"
#include "tools.h"
#include "util.h"

#ifdef _WIN32
#define EXPORT extern "C" __declspec(dllexport)
#else
#define EXPORT extern "C" __attribute__((visibility("default")))
#endif

static bool g_started = false;
static bool g_bootstrap = false;       // started from a listen/singleplayer server state (first run)
static lua_State *g_host_L = nullptr;  // state that runs the pump: menu (client mode) or server (srcds)
static bool g_disabled = false;
static lua_State *g_server_L = nullptr;  // the server state that loaded the module (each new map has a new one)

static std::string command_line() {
    using PlatCmd = const char *(*)();
    auto fn = reinterpret_cast<PlatCmd>(plat::find_symbol(plat::find_loaded_module(plat::tier0_names()), "Plat_GetCommandLine"));
    if (fn) {
        const char *c = fn();
        if (c && *c) return c;
    }
    return plat::process_command_line();
}

static bool global_true(lua_State *L, const char *name) {
    const auto &A = lua::api();
    A.getfield(L, lua::GLOBALSINDEX, name);
    bool b = A.toboolean(L, -1) != 0;
    lua::pop(L, 1);
    return b;
}

static std::string load_or_create_token(std::string &path_out) {
    std::string dir = plat::user_config_dir() + "/gmod-mcp";
    std::string path = dir + "/token";
    path_out = path;
    std::string tok;
    if (plat::read_file(path, tok)) {
        while (!tok.empty() && (tok.back() == '\n' || tok.back() == '\r' || tok.back() == ' ')) tok.pop_back();
        if (tok.size() >= 32) return tok;
    }
    unsigned char raw[32];
    if (!plat::random_bytes(raw, sizeof raw)) return "";
    tok = util::hex(raw, sizeof raw);
    plat::make_dirs(dir);
    if (!plat::write_file(path, tok + "\n")) return "";
    return tok;
}

static int pump_fn(lua_State *L) {
    try {
        agent::pump(L);
    } catch (const std::exception &e) {
        logx::warn("exception in the pump: %s", e.what());
    }
    return 0;
}

static void register_pump(lua_State *L) {
    const auto &A = lua::api();
    A.pushcclosure(L, pump_fn, 0);
    A.setfield(L, lua::GLOBALSINDEX, "__mcpagent_pump");
    std::string e;
    const char *code =
        agent::config().mode == agent::Mode::Client
            ? "hook.Add('Think', '__mcpagent_pump', function() __mcpagent_pump() end)"
            : "hook.Add('Think', '__mcpagent_pump', function() __mcpagent_pump() end)\n"
              "RunConsoleCommand('sv_hibernate_think', '1')";
    if (!lua::run(L, code, "=mcpagent", nullptr, e)) logx::warn("failed to register the pump: %s", e.c_str());
}

static bool start(lua_State *L) {
    std::string e;
    auto args = util::split_args(command_line());
    if (!util::has_flag(args, "-mcp")) {
        logx::info("inactive: the -mcp flag is not on the command line (no server started)");
        return false;
    }
    auto &cfg = agent::config();
    bool menu = global_true(L, "MENU_DLL");
    bool server = global_true(L, "SERVER");
    g_bootstrap = false;
    if (menu) {
        cfg.mode = agent::Mode::Client;
        g_host_L = L;
    } else if (server) {
        std::string ded;
        lua::run(L, "return tostring(game and game.IsDedicated and game.IsDedicated())", "=mcpagent", &ded, e);
        if (ded == "true") {
            cfg.mode = agent::Mode::Dedicated;
            g_host_L = L;
        } else {
            // First run of a singleplayer/listen game: loaded by the server autorun because the main-menu loader is not
            // installed yet. Attach to the menu state of this same process and install the loader for next time.
            lua_State *menu_L = realms::state(realms::MENU);
            if (!menu_L) {
                logx::warn("refused: loaded in a listen/singleplayer server state but no menu state was found");
                return false;
            }
            cfg.mode = agent::Mode::Client;
            g_host_L = menu_L;
            g_bootstrap = true;
        }
    } else {
        logx::warn("refused: the module must be loaded in the menu state or in a server state");
        return false;
    }
    if (!plat::pin_self_module()) logx::warn("unable to pin the module in memory");

    std::string cwd = plat::current_directory();
    cfg.game_dir = cwd + "/garrysmod";
#ifdef _WIN32
    for (auto &c : cfg.game_dir)
        if (c == '/') c = '\\';
    cfg.tmp_dir = cfg.game_dir + "\\lua\\mcpagent_tmp";
    cfg.data_dir = cfg.game_dir + "\\data\\mcpagent";
#else
    cfg.tmp_dir = cfg.game_dir + "/lua/mcpagent_tmp";
    cfg.data_dir = cfg.game_dir + "/data/mcpagent";
#endif
    plat::make_dirs(cfg.tmp_dir);
    plat::make_dirs(cfg.data_dir);

    std::string tok_path;
    cfg.token = load_or_create_token(tok_path);
    if (cfg.token.empty()) {
        logx::warn("refused: unable to read or create the token (%s)", tok_path.c_str());
        return false;
    }

    std::string pstr = util::flag_value(args, "-mcpport", "27080");
    cfg.base_port = std::atoi(pstr.c_str());
    if (cfg.base_port <= 0 || cfg.base_port > 65000) cfg.base_port = 27080;

    // The line observer is set BEFORE the capture is installed: once installed, the engine prints from any thread and
    // calls the observer (a std::function) concurrently; assigning it afterwards would be a data race.
    // Connection failures are shown by a native engine dialog the agent cannot click: surface them as events.
    console::set_line_observer([](const std::string &line) {
        static const std::string a = "Disconnected: ", b = "Connection failed";
        if (line.rfind(a, 0) == 0) events::publish("disconnect_reason", nlohmann::json{{"reason", line.substr(a.size())}});
        else if (line.rfind(b, 0) == 0 || line.find("Bad server address") != std::string::npos)
            events::publish("connection_failed", nlohmann::json{{"message", line}});
    });
    std::string cm = console::install(util::has_flag(args, "-condebug") ? cfg.game_dir + "/console.log" : "");
    if (cfg.mode == agent::Mode::Dedicated) console::set_parse_errors_from_console(true);

    tools::register_all();

    // -mcp_lua_mount "<folder>": a test client started on a worktree's code (instances_launch {lua_mount}).
    std::string ov = util::flag_value(args, "-mcp_lua_mount", "");
    if (!ov.empty()) {
        std::string root, oerr, why;
        if (!lua_mount::normalize_root(ov, root, oerr)) logx::warn("-mcp_lua_mount ignored: %s", oerr.c_str());
        else if (!(oerr = lua_mount::refuse_inside_game(root, cfg.game_dir)).empty()) logx::warn("-mcp_lua_mount ignored: %s", oerr.c_str());
        else if (!lua_mount::supported(&why)) logx::warn("-mcp_lua_mount ignored: %s", why.c_str());
        else {
            nlohmann::json r = lua_mount::set(root, false);
            if (!r.value("ok", false)) logx::warn("-mcp_lua_mount failed: %s", r.dump().c_str());
        }
    }

    cfg.port = httpsrv::start(cfg.base_port, 16, cfg.token);
    if (cfg.port == 0) {
        logx::warn("refused: no free port between %d and %d", cfg.base_port, cfg.base_port + 15);
        return false;
    }
    cfg.instance_id = std::string(agent::mode_name()) + "-" + std::to_string(plat::process_id());
    mcp::set_instructions(resources::instructions());
    coordinator::start();
    events::start_watchdog(5.0);
    logx::info("active: mode %s, instance %s, http://127.0.0.1:%d/mcp (console capture: %s, token: %s)", agent::mode_name(),
               cfg.instance_id.c_str(), cfg.port, cm.c_str(), tok_path.c_str());
    logx::info("to connect Claude Code or another MCP client, type mcpagent_setup in this console");
    if (g_bootstrap) logx::info("first run: loaded by the server autorun, now attached to the main menu state");
    if (cfg.mode == agent::Mode::Client) {
        // Keep the main-menu loader installed and up to date (game updates overwrite menu.lua).
        lua_State *hostL = g_host_L;
        auto compiles = [hostL](const std::string &src, std::string &cerr) {
            const auto &A = lua::api();
            int top = A.gettop(hostL);
            bool ok = A.loadbuffer(hostL, src.data(), src.size(), "=menu.lua") == 0;
            if (!ok) cerr = lua::tostring(hostL, -1);
            A.settop(hostL, top);
            return ok;
        };
        std::string ierr, state = installer::ensure_menu_patch(cfg.game_dir, ierr, compiles);
        if (state.empty()) logx::warn("could not install the main-menu loader: %s", ierr.c_str());
        else if (state != "unchanged") {
            logx::info("main-menu loader %s in lua/menu/menu.lua (from the next game start the module loads at the main menu)",
                       state.c_str());
            agent::notify_player("gmod-mcp is installed and running. To connect Claude Code, open the console (~) and type: mcpagent_setup");
        }
    }
    return true;
}

static int open_impl(lua_State *L);

// Entry point. No exception may reach the game: one would end the game process.
EXPORT int gmod13_open(lua_State *L) {
    try {
        return open_impl(L);
    } catch (const std::exception &ex) {
        logx::warn("not loaded: unexpected error during start-up: %s", ex.what());
    } catch (...) {
        logx::warn("not loaded: unexpected error during start-up");
    }
    // Caught, so not a crash: the crash guard's marker is already gone (unwinding), the next start may try again.
    g_disabled = true;
    return 0;
}

// Removes this process's crash-guard marker when the first load is over, by any path except a crash.
struct CrashGuardEnd {
    std::string dir;
    ~CrashGuardEnd() {
        if (!dir.empty()) crashguard::end(dir, plat::process_id());
    }
};

static int open_impl(lua_State *L) {
    logx::init();
    if (g_disabled) return 0;
    CrashGuardEnd guard;
    if (!g_started && util::has_flag(util::split_args(command_line()), "-mcp")) {
        // If the game crashes during the first load, the next launch skips the module once (no crash loop). One marker
        // per process: test clients started together do not take each other's load for a crash.
        std::string dir = plat::current_directory() + "/garrysmod/data/mcpagent";
        auto r = crashguard::begin(dir, plat::process_id(), plat::process_start_stamp);
        for (const auto &n : r.notes) logx::warn("%s", n.c_str());
        if (r.skip) {
            g_disabled = true;
            return 0;
        }
        guard.dir = dir;
    }
    std::string e;
    if (!lua::load(e) || !realms::init(e)) {
        logx::warn("initialization failed: %s", e.c_str());
        g_disabled = true;
        return 0;
    }
    if (!g_started) {
        bool ok = start(L);
        if (ok) register_pump(g_host_L);
        if (!ok) {
            g_disabled = true;
            return 0;
        }
        g_started = true;
        // Dedicated server or first run from a listen/singleplayer server state: this is the first map's server state.
        if (global_true(L, "SERVER") && !global_true(L, "MENU_DLL")) {
            g_server_L = L;
            agent::on_server_state_open();
        }
        return 0;
    }
    // Already running. Client mode: the module lives in the menu state; another state requiring it (the server autorun
    // of every map) is a no-op. Dedicated mode: the server state is new after each map change, re-register the pump.
    // A new server state (client mode too: the autorun loader requires the module in each map's server state) is a new
    // map load; the earliest moment the module can tell, before the pump sees the realm and before most autorun files.
    if (global_true(L, "SERVER") && !global_true(L, "MENU_DLL")) {
        g_server_L = L;
        agent::on_server_state_open();
    }
    if (agent::config().mode == agent::Mode::Dedicated) {
        g_host_L = L;
        register_pump(L);
    }
    return 0;
}

EXPORT int gmod13_close(lua_State *L) {
    if (!g_started) return 0;
    if (agent::config().mode == agent::Mode::Client && L == g_host_L) {
        // Menu state closing = game shutting down.
        coordinator::stop();
        events::stop_watchdog();
        httpsrv::stop();
        jobs::cancel_all();
        console::uninstall();
        g_started = false;
        g_disabled = true;
    }
    if (L == g_server_L) {
        g_server_L = nullptr;
        agent::on_server_state_close();
    }
    // In srcds, the server state is recreated on every map: the HTTP server keeps running, the pump will be re-registered.
    return 0;
}

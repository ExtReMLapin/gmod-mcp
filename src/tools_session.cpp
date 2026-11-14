// Tools: map_list, map_load, disconnect, game_quit, game_terminate, wait_for, wait_frames.
#include <chrono>
#include <mutex>
#include <thread>

#include "agent.h"
#include "events.h"
#include "jobs.h"
#include "log.h"
#include "mcp.h"
#include "platform/platform.h"
#include "realms.h"
#include "reqctx.h"
#include "tools.h"

namespace tools {

using Clock = std::chrono::steady_clock;

static double since(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

static int host() { return agent::config().mode == agent::Mode::Client ? realms::MENU : realms::SERVER; }

static json map_list_tool(const json &args) {
    json p{{"filter", args.value("filter", "")}, {"limit", args.value("limit", 1000)}};
    return agent::rt(host(), "map_list", p);
}

static json disconnect_tool(const json &args);

// map_load and disconnect change the game state: only one at a time (concurrent calls get "busy").
static std::mutex g_lifecycle;

static json map_load_impl(const json &args);
static json disconnect_impl(const json &args);

static json map_load_tool(const json &args) {
    std::unique_lock<std::mutex> lk(g_lifecycle, std::try_to_lock);
    if (!lk.owns_lock()) return agent::err("busy", "another map_load/disconnect is in progress");
    return map_load_impl(args);
}

static json disconnect_tool(const json &args) {
    std::unique_lock<std::mutex> lk(g_lifecycle, std::try_to_lock);
    if (!lk.owns_lock()) return agent::err("busy", "another map_load/disconnect is in progress");
    return disconnect_impl(args);
}

static json map_load_impl(const json &args) {
    auto t0 = Clock::now();
    std::string map = args["map"];
    double timeout = args.value("timeout_s", 180.0);
    json ex = agent::rt(host(), "map_exists", json{{"map", map}});
    if (!ex.value("ok", false)) return ex;
    if (!ex["result"].value("exists", false)) return agent::err("not_found", "map not found: maps/" + map + ".bsp");
    if (args.contains("maxplayers") && agent::config().mode == agent::Mode::Dedicated)
        return agent::err("invalid_params", "maxplayers cannot be changed at runtime on srcds (launch parameter)");
    // Changing maxplayers mid-game: full disconnect first (the engine refuses otherwise).
    if (args.contains("maxplayers") && agent::realm_present(realms::SERVER)) {
        json sv = agent::rt(realms::SERVER, "status", json::object(), 5.0);
        if (sv.value("ok", false) && sv["result"].value("maxplayers", 0) != args["maxplayers"].get<int>()) {
            json d = disconnect_impl(json{{"timeout_s", 30}});
            if (!d.value("ok", false)) return d;
        }
    }
    // Let the menu's HTML loading screen settle before starting a new load: back-to-back transitions crash the game's
    // embedded Chromium (html_chromium.dll). Wait until the menu is idle, then ~30 frames.
    if (agent::config().mode == agent::Mode::Client) {
        reqctx::progress(since(t0), 0, "waiting for the menu to be idle");
        auto tw = Clock::now();
        while (since(tw) < 15.0) {
            json idle = agent::rt(realms::MENU, "menu_idle", json::object(), 5.0);
            if (idle.value("ok", false) && !idle["result"].value("loading", true)) break;
            plat::sleep_ms(100);
        }
        uint64_t f0 = jobs::frame_count();
        while (jobs::frame_count() < f0 + 30 && since(tw) < 20.0) plat::sleep_ms(10);
    }
    uint64_t gs = agent::generation(realms::SERVER), gc = agent::generation(realms::CLIENT);
    uint64_t cursor = events::cursor_now();
    json p{{"map", map}};
    if (args.contains("gamemode")) p["gamemode"] = args["gamemode"];
    if (args.contains("maxplayers")) p["maxplayers"] = args["maxplayers"];
    json st = agent::rt(host(), "map_start", p);
    if (!st.value("ok", false)) return st;
    reqctx::progress(since(t0), 0, "loading " + map);
    bool dedicated = agent::config().mode == agent::Mode::Dedicated;
    for (;;) {
        if (reqctx::cancelled()) return agent::err("cancelled", "map_load cancelled (the map may keep loading)");
        double left = timeout - since(t0);
        if (left <= 0)
            return agent::err("timeout", "map not ready after " + std::to_string((int)timeout) + " s",
                              json{{"server_available", agent::realm_present(realms::SERVER)},
                                   {"client_available", agent::realm_present(realms::CLIENT)},
                                   {"client_lock", agent::lock_status()}});
        json w = events::wait({"map_loaded"}, cursor, std::min(left, 1.0), 10);
        reqctx::progress(since(t0), 0,
                         "loading " + map + " (" + std::to_string((int)since(t0)) + " s; server " +
                             (agent::realm_present(realms::SERVER) ? "up" : "not yet") + ", client " +
                             (agent::realm_present(realms::CLIENT) ? "up" : "not yet") + ")");
        cursor = w["next_cursor"].get<uint64_t>();
        for (const auto &e : w["events"]) {
            const json &d = e["data"];
            bool new_sv = d.value("server_generation", (uint64_t)0) > gs;
            bool new_cl = dedicated || d.value("client_generation", (uint64_t)0) > gc;
            if (new_sv && new_cl) {
                json r = d;
                r["duration_s"] = since(t0);
                r["requested_map"] = map;
                if (d.value("map", "") != map) r["warning"] = "the loaded map does not match the requested map";
                return ok(r);
            }
        }
    }
}

static json disconnect_impl(const json &args) {
    if (agent::config().mode != agent::Mode::Client) return agent::err("unsupported", "disconnect: client mode only");
    auto t0 = Clock::now();
    double timeout = args.value("timeout_s", 30.0);
    bool was = agent::realm_present(realms::CLIENT) || agent::realm_present(realms::SERVER);
    json r = agent::rt(realms::MENU, "disconnect", json::object());
    if (!r.value("ok", false)) return r;
    while (since(t0) < timeout) {
        if (!agent::realm_present(realms::CLIENT) && !agent::realm_present(realms::SERVER) && jobs::seconds_since_last_frame() < 1.0)
            return ok(json{{"disconnected", true}, {"was_in_game", was}, {"duration_s", since(t0)},
                           {"realms", {{"client", false}, {"server", false}, {"menu", agent::realm_present(realms::MENU)}}}});
        if (reqctx::cancelled()) return agent::err("cancelled", "disconnect cancelled");
        plat::sleep_ms(50);
    }
    return agent::err("timeout", "client/server realms still present after " + std::to_string((int)timeout) + " s");
}

static json quit_tool(const json &) {
    json r = agent::rt(host(), "quit", json::object(), 5.0);
    if (!r.value("ok", false)) return r;
    return ok(json{{"quitting", true}, {"note", "clean shutdown requested; the MCP server will go away with the process"}});
}

static json terminate_tool(const json &args) {
    int delay = args.value("delay_ms", 200);
    uint32_t pid = plat::process_id();
    logx::warn("game_terminate: immediate termination of process %u in %d ms", pid, delay);
    // Runs from the network thread: works even if the main thread is frozen.
    std::thread([delay] {
        plat::sleep_ms(delay);
        plat::terminate_process(1);
    }).detach();
    return ok(json{{"terminating", true}, {"pid", pid}, {"main_thread_stalled", jobs::seconds_since_last_frame() > 5.0},
                   {"last_tick_age_s", jobs::seconds_since_last_frame()}});
}

static json wait_for_tool(const json &args) {
    auto t0 = Clock::now();
    int r = realm_of(args, "realm", realms::SERVER);
    if (r < 0) return agent::err("invalid_params", "unknown realm");
    double timeout = args.value("timeout_s", 10.0);
    double interval = args.value("interval_s", 0.0);
    std::string code = args["condition"];
    if (!agent::realm_present(r)) return agent::err("realm_unavailable", std::string("realm ") + realms::name(r) + " unavailable");
    uint64_t gen0 = agent::generation(r);
    int polls = 0;
    for (;;) {
        if (reqctx::cancelled()) return agent::err("cancelled", "wait_for cancelled");
        double left = timeout - since(t0);
        json res = agent::rt(r, "eval_cond", json{{"code", code}}, std::max(0.5, std::min(left, 5.0)));
        polls++;
        if (!agent::realm_present(r) || (agent::generation(r) != gen0 && gen0 != 0))
            return agent::err("realm_lost", std::string("realm ") + realms::name(r) + " disappeared or was recreated during the wait",
                              json{{"elapsed_s", since(t0)}, {"polls", polls}});
        if (!res.value("ok", false)) {
            std::string code_e = res["error"].value("code", "");
            if (code_e == "realm_unavailable" || code_e == "runtime_unavailable")
                return agent::err("realm_lost", res["error"].value("message", ""), json{{"elapsed_s", since(t0)}});
            if (code_e != "main_thread_timeout") return res;
        } else if (res["result"].value("truthy", false)) {
            return ok(json{{"satisfied", true}, {"elapsed_s", since(t0)}, {"polls", polls}, {"value", res["result"]["value"]}});
        }
        if (polls % 30 == 1) reqctx::progress(since(t0), timeout, "condition still false after " + std::to_string(polls) + " polls");
        if (since(t0) >= timeout)
            return agent::err("timeout", "condition still false after " + std::to_string(timeout) + " s",
                              json{{"polls", polls}, {"last_value", res.value("ok", false) ? res["result"]["value"] : json()}});
        if (interval > 0) plat::sleep_ms((int)(interval * 1000));
        else {
            uint64_t f = jobs::frame_count();  // wait for the next frame
            auto tw = Clock::now();
            while (jobs::frame_count() == f && since(tw) < 0.5) plat::sleep_ms(2);
        }
    }
}

static json wait_frames_tool(const json &args) {
    auto t0 = Clock::now();
    uint64_t n = args["n"].get<uint64_t>();
    double timeout = args.value("timeout_s", 30.0);
    uint64_t start = jobs::frame_count(), reported = 0;
    while (jobs::frame_count() < start + n) {
        if (since(t0) > timeout)
            return agent::err("timeout", "only " + std::to_string(jobs::frame_count() - start) + " frames in " + std::to_string(timeout) + " s");
        if (reqctx::cancelled()) return agent::err("cancelled", "wait_frames cancelled");
        uint64_t done = jobs::frame_count() - start;
        if (n >= 120 && done / 60 != reported / 60) {
            reported = done;
            reqctx::progress((double)done, (double)n, "frames");
        }
        plat::sleep_ms(2);
    }
    return ok(json{{"frames", jobs::frame_count() - start}, {"elapsed_s", since(t0)}});
}

void register_session() {
    json RO = {{"readOnlyHint", true}};
    mcp::register_tool({"map_list", "List maps", "Installed maps: every maps/*.bsp on the game's GAME path (garrysmod, addons, workshop, mounted games such as "
                        "HL2), names without .bsp, sorted, filtered by a case-insensitive substring. Same answer on every instance of "
                        "this game folder (callable without `instance`). Result: {maps: [names], count (all matches), truncated, filter}. "
                        "Example: {\"filter\": \"gm_\"}.",
                        json{{"properties", {{"filter", {{"type", "string"}, {"description", "Substring (empty = all)."}}},
                                             {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10000}, {"description", "Default 1000."}}}}}},
                        RO, map_list_tool});
    mcp::register_tool({"map_load", "Load a map",
                        "Loads a map and WAITS until it is playable: client mode = the console `map` command (singleplayer when "
                        "maxplayers is 1, listen server otherwise), then it waits until NEW server and client realms are up and "
                        "ready (gamemode loaded, local player valid) and the loading screen is gone; srcds = `changelevel`, server "
                        "realm only. Every Lua state of the previous map is destroyed: its globals, hooks, timers, jobs and debug "
                        "drawings are gone, and the addons' files are loaded again from disk. Changing maxplayers on a running "
                        "game disconnects first (client mode only). Result: {map, gamemode, maxplayers, requested_map, duration_s, "
                        "server_generation, client_generation, warning?}. Errors: not_found (no maps/<map>.bsp), busy (another "
                        "map_load/disconnect runs), timeout (with which realms are up), cancelled. IMPORTANT: when your client "
                        "runs it as a background MCP task, wait for its result before calling a realm tool: a call made before it "
                        "ends reaches the OLD map's state. Every lua_exec/lua_job result carries the realm's `generation`: equal to "
                        "server_generation/client_generation = the new map's state. Example: {\"map\": \"gm_construct\"}.",
                        json{{"properties",
                              {{"map", {{"type", "string"}, {"minLength", 1}, {"description", "Name without extension, e.g. gm_construct."}}},
                               {"gamemode", {{"type", "string"}, {"description", "Gamemode (e.g. sandbox). Default: unchanged."}}},
                               {"maxplayers", {{"type", "integer"}, {"minimum", 1}, {"maximum", 128}, {"description", "1 = singleplayer, >1 = listen server (needed for bots_add). Default: unchanged."}}},
                               {"timeout_s", {{"type", "number"}, {"minimum", 5}, {"maximum", 900}, {"description", "Max wait in seconds (default 180)."}}}}},
                             {"required", {"map"}}},
                        json{{"destructiveHint", true}}, map_load_tool});
    mcp::set_task_support("map_load", "optional");
    mcp::register_tool({"disconnect", "Return to main menu", "Leaves the current map for the main menu (console `disconnect`) and waits until the client and server realms "
                        "are gone; their Lua states are destroyed. Client mode only (unsupported on srcds). Result: {disconnected, "
                        "was_in_game, duration_s, realms}. Errors: busy (a map_load runs), timeout.",
                        json{{"properties", {{"timeout_s", {{"type", "number"}, {"minimum", 1}, {"maximum", 120}, {"description", "Default 30."}}}}}},
                        json{{"destructiveHint", true}, {"idempotentHint", true}}, disconnect_tool});
    mcp::register_tool({"game_quit", "Quit the game cleanly", "Asks the game to quit cleanly (console `quit`): it saves its config (cfg/config.cfg, video settings) and exits; "
                        "the MCP server of this instance goes away with it. For test clients started by instances_launch, use "
                        "instances_close instead (a clean quit writes their small window size into the user's video settings). "
                        "Result: {quitting: true}.",
                        json::object(), json{{"destructiveHint", true}}, quit_tool});
    mcp::register_tool({"game_terminate", "Kill the game immediately",
                        "Kills this game process at once (no config saved, no clean shutdown), from the network thread, so it works "
                        "even when the game is frozen (infinite loop, status main_thread_stalled). The answer {terminating, pid, "
                        "main_thread_stalled, last_tick_age_s} is sent first, then the process ends after delay_ms. With `instance`, "
                        "the coordinator relays it to that game. Never use it on the user's own game unless they ask.",
                        json{{"properties", {{"delay_ms", {{"type", "integer"}, {"minimum", 0}, {"maximum", 5000}, {"description", "Delay before termination (default 200, to let the response go out)."}}}}}},
                        json{{"destructiveHint", true}}, terminate_tool});
    mcp::register_tool({"wait_for", "Wait for a Lua condition",
                        "Waits until a Lua condition is true. `condition` is an expression (\"IsValid(Entity(57))\") or a block "
                        "with return (\"local d = Entity(57) return IsValid(d) and d:GetInternalVariable('m_eDoorState') == 2\"), "
                        "compiled once and evaluated in `realm` every frame (or every interval_s); true = anything but nil/false. "
                        "Each evaluation may run 1 s at most. Result: {satisfied: true, elapsed_s, polls, value (the last value)}. "
                        "Errors: timeout (details: polls, last_value), realm_lost (the realm was destroyed or recreated, e.g. a map "
                        "change, during the wait), realm_unavailable, lua_syntax_error, lua_error. Reports progress; can run as an "
                        "MCP task.",
                        json{{"properties",
                              {{"realm", realm_prop("Evaluation realm (default server).")},
                               {"condition", {{"type", "string"}, {"description", "Lua expression or block with return, e.g. 'IsValid(Entity(1)) and Entity(1):Alive()'."}}},
                               {"timeout_s", {{"type", "number"}, {"minimum", 0}, {"maximum", 600}, {"description", "Max wait in seconds (default 10)."}}},
                               {"interval_s", {{"type", "number"}, {"minimum", 0}, {"maximum", 60}, {"description", "Seconds between evaluations (default 0 = every frame)."}}}}},
                             {"required", {"condition"}}},
                        RO, wait_for_tool});
    mcp::set_task_support("wait_for", "optional");
    mcp::register_tool({"wait_frames", "Wait N frames", "Waits until the game's main loop has run n more frames (counted by gmod-mcp every frame, map or not), e.g. to let "
                        "a console command, a spawn or a SetPos take effect. Result: {frames, elapsed_s}. Error timeout if the game "
                        "renders fewer frames in timeout_s (frozen, minimized with a low fps_max_nofocus). A light test client runs "
                        "at 20 fps: 20 frames = 1 s.",
                        json{{"properties", {{"n", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100000}, {"description", "Number of frames to wait."}}},
                                             {"timeout_s", {{"type", "number"}, {"minimum", 0.1}, {"maximum", 600}, {"description", "Max wait in seconds (default 30)."}}}}},
                             {"required", {"n"}}},
                        RO, wait_frames_tool});
}

}  // namespace tools

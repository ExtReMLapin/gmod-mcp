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
#include "tasks.h"
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
    uint64_t gs = agent::generation(realms::SERVER), gc = agent::generation(realms::CLIENT);
    uint64_t cursor = events::cursor_now();
    json p{{"map", map}};
    if (args.contains("gamemode")) p["gamemode"] = args["gamemode"];
    if (args.contains("maxplayers")) p["maxplayers"] = args["maxplayers"];
    json st = agent::rt(host(), "map_start", p);
    if (!st.value("ok", false)) return st;
    bool dedicated = agent::config().mode == agent::Mode::Dedicated;
    for (;;) {
        if (tasks::should_stop()) return agent::err("cancelled", "map_load cancelled (the map may keep loading)");
        double left = timeout - since(t0);
        if (left <= 0)
            return agent::err("timeout", "map not ready after " + std::to_string((int)timeout) + " s",
                              json{{"server_available", agent::realm_present(realms::SERVER)},
                                   {"client_available", agent::realm_present(realms::CLIENT)},
                                   {"client_lock", agent::lock_status()}});
        json w = events::wait({"map_loaded"}, cursor, std::min(left, 1.0), 10);
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
        if (tasks::should_stop()) return agent::err("cancelled", "disconnect cancelled");
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
        if (tasks::should_stop()) return agent::err("cancelled", "wait_for cancelled");
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
    uint64_t start = jobs::frame_count();
    while (jobs::frame_count() < start + n) {
        if (since(t0) > timeout)
            return agent::err("timeout", "only " + std::to_string(jobs::frame_count() - start) + " frames in " + std::to_string(timeout) + " s");
        if (tasks::should_stop()) return agent::err("cancelled", "wait_frames cancelled");
        plat::sleep_ms(2);
    }
    return ok(json{{"frames", jobs::frame_count() - start}, {"elapsed_s", since(t0)}});
}

void register_session() {
    json RO = {{"readOnlyHint", true}};
    mcp::register_tool({"map_list", "List maps", "Installed maps (maps/*.bsp on the GAME path, mounted games included), filtered by case-insensitive substring.",
                        json{{"properties", {{"filter", {{"type", "string"}, {"description", "Substring (empty = all)."}}},
                                             {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10000}, {"description", "Default 1000."}}}}}},
                        RO, map_list_tool});
    mcp::register_tool({"map_load", "Load a map",
                        "Loads a map (singleplayer if maxplayers=1, listen server otherwise) and WAITS until the server AND client realms "
                        "are recreated and ready (gamemode loaded, valid local player). Clean error if the map does not exist. "
                        "Equivalent to waiting for the map_loaded event. Can run as an MCP task.",
                        json{{"properties",
                              {{"map", {{"type", "string"}, {"minLength", 1}, {"description", "Name without extension, e.g. gm_construct."}}},
                               {"gamemode", {{"type", "string"}, {"description", "Gamemode (e.g. sandbox). Default: unchanged."}}},
                               {"maxplayers", {{"type", "integer"}, {"minimum", 1}, {"maximum", 128}, {"description", "1 = singleplayer, >1 = listen server."}}},
                               {"timeout_s", {{"type", "number"}, {"minimum", 5}, {"maximum", 900}, {"description", "Default 180."}}}}},
                             {"required", {"map"}}},
                        json{{"destructiveHint", true}}, map_load_tool});
    mcp::set_task_support("map_load", "optional");
    mcp::register_tool({"disconnect", "Return to main menu", "Disconnects (disconnect command) and waits until the client and server realms are gone.",
                        json{{"properties", {{"timeout_s", {{"type", "number"}, {"minimum", 1}, {"maximum", 120}, {"description", "Default 30."}}}}}},
                        json{{"destructiveHint", true}, {"idempotentHint", true}}, disconnect_tool});
    mcp::register_tool({"game_quit", "Quit the game cleanly", "Requests a clean shutdown (quit command). The MCP server goes away with the process.",
                        json::object(), json{{"destructiveHint", true}}, quit_tool});
    mcp::register_tool({"game_terminate", "Kill the game immediately",
                        "Immediate process termination, run from the network thread: works even if the main thread is frozen "
                        "(infinite loop). Use game_quit normally.",
                        json{{"properties", {{"delay_ms", {{"type", "integer"}, {"minimum", 0}, {"maximum", 5000}, {"description", "Delay before termination (default 200, to let the response go out)."}}}}}},
                        json{{"destructiveHint", true}}, terminate_tool});
    mcp::register_tool({"wait_for", "Wait for a Lua condition",
                        "Re-evaluates a Lua expression (or a block with return) in a realm every frame (or every interval_s) "
                        "until it is true. Errors: timeout, realm_lost (realm destroyed/recreated during the wait), lua_error.",
                        json{{"properties",
                              {{"realm", realm_prop("Evaluation realm (default server).")},
                               {"condition", {{"type", "string"}, {"description", "E.g. 'IsValid(Entity(1)) and Entity(1):Alive()'."}}},
                               {"timeout_s", {{"type", "number"}, {"minimum", 0}, {"maximum", 600}, {"description", "Default 10."}}},
                               {"interval_s", {{"type", "number"}, {"minimum", 0}, {"maximum", 60}, {"description", "Default: every frame."}}}}},
                             {"required", {"condition"}}},
                        RO, wait_for_tool});
    mcp::set_task_support("wait_for", "optional");
    mcp::register_tool({"wait_frames", "Wait N frames", "Waits until the main thread has run N frames (menu state pump).",
                        json{{"properties", {{"n", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100000}, {"description", "Number of frames."}}},
                                             {"timeout_s", {{"type", "number"}, {"minimum", 0.1}, {"maximum", 600}, {"description", "Default 30."}}}}},
                             {"required", {"n"}}},
                        RO, wait_frames_tool});
}

}  // namespace tools

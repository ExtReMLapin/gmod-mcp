// Tools: hook_list, hook_trace, perf_hook_cost, tests_run (+ sound_trace relies on trace_hook).
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <regex>

#include "agent.h"
#include "mcp.h"
#include "platform/platform.h"
#include "realms.h"
#include "reqctx.h"
#include "tools.h"

namespace tools {

using Clock = std::chrono::steady_clock;

// Traces an event for duration_s (or until max_calls): start, poll, stop. The tracer is always removed.
json trace_hook(int realm, const std::string &start_tool, const json &start_args, double duration_s, int max_calls) {
    json st = agent::rt(realm, start_tool, start_args);
    if (!st.value("ok", false)) return st;
    std::string id = st["result"]["id"];
    uint64_t gen = agent::generation(realm);
    auto t0 = Clock::now();
    while (std::chrono::duration<double>(Clock::now() - t0).count() < duration_s) {
        if (reqctx::cancelled()) break;
        plat::sleep_ms(100);
        if (agent::generation(realm) != gen || !agent::realm_present(realm))
            return agent::err("realm_lost", "the realm was destroyed during the trace (the tracer went away with it)");
        json p = agent::rt(realm, "trace_poll", json{{"id", id}}, 5.0);
        int recorded = p.value("ok", false) ? p["result"].value("recorded", 0) : 0;
        double el = std::chrono::duration<double>(Clock::now() - t0).count();
        reqctx::progress(el, duration_s, std::to_string(recorded) + " calls recorded");
        if (recorded >= max_calls) break;
    }
    return agent::rt(realm, "trace_stop", json{{"id", id}});
}

static json hook_trace_tool(const json &args) {
    int r = realm_of(args, "realm", realms::SERVER);
    if (r < 0) return agent::err("invalid_params", "unknown realm");
    int max = args.value("max_calls", 100);
    json sa{{"event", args["event"]}, {"max_calls", max}, {"depth", args.value("depth", 2)}};
    return trace_hook(r, "trace_start", sa, args.value("duration_s", 5.0), max);
}

static json perf_hook_cost_tool(const json &args) {
    int r = realm_of(args, "realm", realms::SERVER);
    if (r < 0) return agent::err("invalid_params", "unknown realm");
    json st = agent::rt(r, "perf_start", json{{"event", args["event"]}});
    if (!st.value("ok", false)) return st;
    std::string id = st["result"]["id"];
    uint64_t gen = agent::generation(r);
    auto t0 = Clock::now();
    double dur = args.value("duration_s", 5.0);
    while (std::chrono::duration<double>(Clock::now() - t0).count() < dur && !reqctx::cancelled()) {
        plat::sleep_ms(50);
        if (agent::generation(r) != gen) return agent::err("realm_lost", "the realm was recreated during the measurement");
    }
    return agent::rt(r, "perf_stop", json{{"id", id}});
}

// Resolves a path or a pattern (* and ** in the file name) relative to tmp_dir, or absolute.
static std::vector<std::string> resolve_glob(const std::string &pattern, std::string &err) {
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    fs::path p = fs::u8path(pattern);
    fs::path base = p.is_absolute() ? p.parent_path() : fs::u8path(agent::config().tmp_dir) / p.parent_path();
    std::string name = p.filename().u8string();
    if (name.find('*') == std::string::npos) {
        fs::path f = base / name;
        if (fs::exists(f)) out.push_back(f.u8string());
        else err = "not found: " + f.u8string();
        return out;
    }
    bool recursive = pattern.find("**") != std::string::npos;
    std::string rx = "^";
    for (char c : name) {
        if (c == '*') rx += ".*";
        else if (std::string(".+?^$()[]{}|\\").find(c) != std::string::npos) rx += std::string("\\") + c;
        else rx += c;
    }
    std::regex re(rx + "$", std::regex::icase);
    if (recursive && base.filename() == "**") base = base.parent_path();
    for (const auto &rel : plat::list_files(base.u8string(), recursive)) {
        std::string fname = fs::u8path(rel).filename().u8string();
        if (std::regex_match(fname, re)) out.push_back((base / fs::u8path(rel)).u8string());
    }
    std::sort(out.begin(), out.end());
    if (out.empty()) err = "no file matches " + pattern + " (base " + base.u8string() + ")";
    return out;
}

static json tests_run_tool(const json &args) {
    std::string err;
    auto files = resolve_glob(args["path"], err);
    if (files.empty()) return agent::err("not_found", err);
    json arr = json::array();
    for (const auto &f : files) {
        std::string src, abs, chunk, e;
        if (!resolve_lua_file(f, abs, chunk, e) || !plat::read_file(abs, src)) return agent::err("io_error", "cannot read: " + f);
        arr.push_back(json{{"name", abs}, {"chunkname", chunk}, {"source", src}});
    }
    double budget = args.value("budget", 10.0);
    json r = rt_realm(args, "realm", realms::SERVER, "tests_run", json{{"files", arr}, {"budget", budget}},
                      budget * (double)(files.size() * 20 + 5));
    return r;
}

void register_lua_tools() {
    json RO = {{"readOnlyHint", true}};
    mcp::register_tool({"hook_list", "List hooks", "Hooks registered with hook.Add in a realm (hook.GetTable), with where each function was defined. Without `event`: "
                        "every event (large). Result: {hooks: {<event>: [{id, source (file), line, what}]}, count}, and with "
                        "`event`: event, known (false = no hook and no gamemode function for that name: probably a typo) and "
                        "gamemode_function (source of GAMEMODE:<event>, or null). Example: {\"realm\": \"client\", \"event\": \"Think\"}.",
                        json{{"properties", {{"realm", realm_prop("Default server.")}, {"event", {{"type", "string"}, {"description", "Event name (e.g. Think)."}}}}}},
                        RO, [](const json &a) {
                            json p = json::object();
                            if (a.contains("event")) p["event"] = a["event"];
                            return rt_realm(a, "realm", realms::SERVER, "hook_list", p);
                        }});
    mcp::register_tool({"hook_trace", "Trace a hook",
                        "Records the calls of a hook event for duration_s (or until max_calls), with each call's arguments "
                        "serialized (entities as {__type, id, class...}), through a temporary hook.Add that returns nothing (game "
                        "behaviour unchanged), removed at the end. Waits the whole duration (progress reported; MCP task possible). "
                        "Result: {event, calls: [{t (seconds since start), args: [...]}], recorded, total_calls (also those past "
                        "max_calls), capped, duration_s, tracer_removed}. Not seen: calls that a hook running before it stopped "
                        "by returning a value. Error realm_lost if the map changes meanwhile. Example: {\"realm\": \"server\", "
                        "\"event\": \"PlayerSay\", \"duration_s\": 10}.",
                        json{{"properties",
                              {{"realm", realm_prop("Default server.")},
                               {"event", {{"type", "string"}, {"minLength", 1}, {"description", "E.g. EntityEmitSound, PlayerSpawn, Think."}}},
                               {"duration_s", {{"type", "number"}, {"minimum", 0.1}, {"maximum", 600}, {"description", "Default 5."}}},
                               {"max_calls", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10000}, {"description", "Cap on recorded calls (default 100)."}}},
                               {"depth", {{"type", "integer"}, {"minimum", 0}, {"maximum", 6}, {"description", "Argument serialization depth (default 2)."}}}}},
                             {"required", {"event"}}},
                        RO, hook_trace_tool});
    mcp::set_task_support("hook_trace", "optional");
    mcp::register_tool({"perf_hook_cost", "Cost of a hook's functions",
                        "Measures over duration_s the time spent in each function of a hook event: every hook.Add function "
                        "registered when it starts, and GAMEMODE:<event>. Each one is wrapped with a timer for the measurement and "
                        "put back afterwards (restored = how many hooks were put back; a hook replaced meanwhile is left alone). "
                        "Waits the whole duration. Result: {event, duration_s, functions: [{id, calls, total_ms, avg_ms, max_ms, "
                        "src {source, line}}] sorted by total_ms, most expensive first, restored}. The game runs at its real frame "
                        "rate: on a light test client (20 fps) the number of calls is lower than in a normal game. "
                        "Example: {\"realm\": \"client\", \"event\": \"Think\", \"duration_s\": 10} (`event` is required; "
                        "the length is duration_s, not seconds).",
                        json{{"properties",
                              {{"realm", realm_prop("Default server.")},
                               {"event", {{"type", "string"}, {"minLength", 1}, {"description", "Hook event name, e.g. 'Think', 'Tick', 'PlayerTick'."}}},
                               {"duration_s", {{"type", "number"}, {"minimum", 0.1}, {"maximum", 600}, {"description", "Measurement length in seconds (default 5)."}}}}},
                             {"required", {"event"}}},
                        RO, perf_hook_cost_tool});
    mcp::set_task_support("perf_hook_cost", "optional");
    mcp::register_tool({"tests_run", "Run Lua tests",
                        "Runs test files in GLuaTest format in one realm and returns structured results. A file returns {groupName, "
                        "beforeAll, beforeEach, afterEach, afterAll, cases = {{name, func, skip?, when?, timeout?}}}; inside, "
                        "expect(x).to.equal(y) / .notTo / .beTrue / .beNil / .exist / .beA(type) / .beValid / .beGreaterThan / "
                        ".beLessThan / .beBetween / .aboutEqual(x, tol) / .haveLength / .contain / .err / .errWith(msg) / .succeed. "
                        "Steps share a `state` table (their first argument). `path`: a file or a pattern in the file name (*.lua, "
                        "or **/*.lua for subfolders), relative to lua/mcpagent_tmp/ or absolute. Result: {files: [{file, group, "
                        "cases: [{name, status: pass|fail|error|skip, message?, line?, traceback?, duration_ms}], error?, "
                        "setup_error?}], totals {pass, fail, error, skip}, ok}. The whole run is one call on the main thread (the "
                        "game is frozen meanwhile): for multi-frame tests use lua_job. Full format: resource "
                        "mcpagent://tests-format. Example: {\"realm\": \"server\", \"path\": \"tests/*.lua\"}.",
                        json{{"properties",
                              {{"realm", realm_prop("Default server.")},
                               {"path", {{"type", "string"}, {"minLength", 1}, {"description", "File or pattern, relative to lua/mcpagent_tmp/ or absolute."}}},
                               {"budget", {{"type", "number"}, {"minimum", 0.1}, {"maximum", 120}, {"description", "Time limit per step (load, beforeAll, each case...) in seconds (default 10)."}}}}},
                             {"required", {"path"}}},
                        json{{"destructiveHint", true}}, tests_run_tool});
}

}  // namespace tools

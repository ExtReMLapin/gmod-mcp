// Tools: hook_list, hook_trace, perf_hook_cost, tests_run (+ sound_trace relies on trace_hook).
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <regex>

#include "agent.h"
#include "jobs.h"
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

static const char *kPathHint =
    "\nA relative path is looked up in lua/mcpagent_tmp/ first, then in the mounted Lua folder (lua_mount), the game folder and "
    "include()-style folders; for a file in your project folder, pass its absolute path.";

// Resolves a path or a pattern (* and ** in the file name). A path, or the folder of a pattern, is found like lua_exec_file finds a
// file: absolute, or relative to lua/mcpagent_tmp/, the mounted Lua folder, the game folder.
std::vector<std::string> resolve_test_files(const std::string &pattern, std::string &err) {
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    std::string pat = pattern;
    std::replace(pat.begin(), pat.end(), '\\', '/');
    fs::path p = fs::u8path(pat);
    std::string name = p.filename().u8string();
    if (name.find('*') == std::string::npos) {
        std::string abs;
        if (resolve_lua_path(pat, false, abs, err)) out.push_back(abs);
        else err += kPathHint;
        return out;
    }
    bool recursive = pat.find("**") != std::string::npos;
    fs::path parent = p.parent_path();
    if (parent.filename() == "**") parent = parent.parent_path();
    fs::path base;
    if (p.is_absolute()) {
        base = parent;
    } else if (parent.empty()) {
        base = fs::u8path(agent::config().tmp_dir);
    } else {
        std::string abs;
        if (!resolve_lua_path(parent.generic_u8string(), true, abs, err)) {
            err += kPathHint;
            return out;
        }
        base = fs::u8path(abs);
    }
    std::string rx = "^";
    for (char c : name) {
        if (c == '*') rx += ".*";
        else if (std::string(".+?^$()[]{}|\\").find(c) != std::string::npos) rx += std::string("\\") + c;
        else rx += c;
    }
    std::regex re(rx + "$", std::regex::icase);
    for (const auto &rel : plat::list_files(base.u8string(), recursive)) {
        std::string fname = fs::u8path(rel).filename().u8string();
        if (std::regex_match(fname, re)) out.push_back((base / fs::u8path(rel)).u8string());
    }
    std::sort(out.begin(), out.end());
    if (out.empty()) err = "no file matches " + pat + " (folder " + base.u8string() + ")" + kPathHint;
    return out;
}

// Adds up several tests_run results of the runtime ({files, totals, ok}) into one.
json merge_test_runs(const std::vector<json> &runs) {
    json files = json::array();
    json totals{{"pass", 0}, {"fail", 0}, {"error", 0}, {"skip", 0}};
    bool all_ok = true;
    for (const auto &r : runs) {
        if (r.contains("files") && r["files"].is_array())
            for (const auto &f : r["files"]) files.push_back(f);
        if (r.contains("totals") && r["totals"].is_object())
            for (const char *k : {"pass", "fail", "error", "skip"}) totals[k] = totals[k].get<int>() + r["totals"].value(k, 0);
        all_ok = all_ok && r.value("ok", false);
    }
    return json{{"files", files}, {"totals", totals}, {"ok", all_ok}};
}

static void wait_one_frame() {
    uint64_t f = jobs::frame_count();
    auto t0 = Clock::now();
    while (jobs::frame_count() == f && !reqctx::cancelled() && std::chrono::duration<double>(Clock::now() - t0).count() < 5.0) plat::sleep_ms(2);
}

// One realm: the whole run in one call (all files in the same frame), or, with frames > 0, one call per file with that many
// frames in between (what an Entity:Remove() of one file needs to be seen by the next). Returns an agent result.
static json run_tests_in(int realm, const json &files, const json &extra, double budget, double timeout_s, int frames) {
    auto call = [&](const json &subset) {
        json payload = extra;
        payload["files"] = subset;
        double t = timeout_s > 0 ? timeout_s : (budget > 0 ? budget * (double)(subset.size() * 20 + 5) : 60.0);
        return agent::rt(realm, "tests_run", payload, t);
    };
    if (frames <= 0 || files.size() <= 1) return call(files);
    std::vector<json> runs;
    for (size_t i = 0; i < files.size(); ++i) {
        if (i > 0) {
            if (reqctx::cancelled()) return agent::err("cancelled", "tests_run cancelled");
            for (int k = 0; k < frames; ++k) wait_one_frame();
        }
        json r = call(json::array({files[i]}));
        if (!r.value("ok", false)) return r;
        runs.push_back(r["result"]);
    }
    return ok(merge_test_runs(runs));
}

static json tests_run_tool(const json &args) {
    std::string err;
    auto files = resolve_test_files(args["path"], err);
    if (files.empty()) return agent::err("not_found", err);
    json arr = json::array();
    for (const auto &f : files) {
        std::string src, abs, chunk, e;
        if (!resolve_lua_file(f, abs, chunk, e) || !plat::read_file(abs, src)) return agent::err("io_error", "cannot read: " + f);
        arr.push_back(json{{"name", abs}, {"chunkname", chunk}, {"source", src}});
    }
    double budget = args.value("budget", 10.0);
    json extra{{"budget", budget}};
    for (const char *k : {"case", "only_failures", "output"})
        if (args.contains(k)) extra[k] = args[k];
    double timeout_s = args.value("timeout_s", 0.0);
    int frames = args.value("frames_between_files", 0);
    std::string realm_name = args.value("realm", std::string());
    if (realm_name != "both") {
        int r = realm_of(args, "realm", realms::SERVER);
        if (r < 0) return agent::err("invalid_params", "realm: unknown realm");
        return project_keys(run_tests_in(r, arr, extra, budget, timeout_s, frames), args);
    }
    // realm "both": the server, then the client; every file is listed with its realm. A realm that cannot run (no client on a
    // dedicated server, a locked client) is named in `realms` and makes ok false.
    json realm_info = json::object(), all_files = json::array();
    std::vector<json> runs;
    json first_error;
    for (int r : {realms::SERVER, realms::CLIENT}) {
        const char *rn = r == realms::SERVER ? "server" : "client";
        json res = run_tests_in(r, arr, extra, budget, timeout_s, frames);
        if (!res.value("ok", false)) {
            if (first_error.is_null()) first_error = res;
            realm_info[rn] = json{{"error", res.value("error", json::object())}};
            continue;
        }
        json run = res["result"];
        for (auto &f : run["files"]) f["realm"] = rn;
        realm_info[rn] = json{{"totals", run["totals"]}, {"ok", run.value("ok", false)}};
        runs.push_back(run);
    }
    if (runs.empty()) return first_error;
    json merged = merge_test_runs(runs);
    if (!first_error.is_null()) merged["ok"] = false;
    merged["realms"] = realm_info;
    return project_keys(ok(merged), args);
}

void register_lua_tools() {
    json RO = {{"readOnlyHint", true}};
    mcp::register_tool({"hook_list", "List hooks", "Hooks registered with hook.Add in a realm (hook.GetTable), with where each function was defined. Without `event`: "
                        "every event (large; `name` keeps the hooks whose identifier contains it, e.g. to check that one was removed). Result: {hooks: {<event>: [{id, source (file), line, what}]}, count}, and with "
                        "`event`: event, known (false = no hook and no gamemode function for that name: probably a typo) and "
                        "gamemode_function (source of GAMEMODE:<event>, or null). Example: {\"realm\": \"client\", \"event\": \"Think\"}.",
                        json{{"properties", {{"realm", realm_prop("Default server.")}, {"event", {{"type", "string"}, {"description", "Event name (e.g. Think)."}}},
                                             {"name", {{"type", "string"}, {"description", "Keep only the hooks whose identifier contains this text (plain text, any case, not a pattern); events left empty are omitted."}}}}}},
                        RO, [](const json &a) {
                            json p = json::object();
                            if (a.contains("event")) p["event"] = a["event"];
                            if (a.contains("name")) p["name"] = a["name"];
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
                        "beforeAll, beforeEach, afterEach, afterAll, cases = {{name, func, skip?, when?, timeout?, timing?}}}; inside, "
                        "expect(x).to.equal(y) / .notTo / .beTrue / .beNil / .exist / .beA(type) / .beValid / .beGreaterThan / "
                        ".beLessThan / .beBetween / .aboutEqual(x, tol) / .haveLength / .contain / .err / .errWith(msg) / .succeed. "
                        "A failed expect() raises a string \"file:line: expected ...\" (so assert(ok, err) keeps it). Skip a case with "
                        "skip = \"reason\", when = false, or expect.skip(reason) inside it (status skip, with the reason). "
                        "`state` (first argument of every step) is ONE table per file, shared by beforeAll, beforeEach, the cases, "
                        "afterEach and afterAll, and NOT reset between cases: reset what you need in beforeEach. "
                        "`path`: a file or a pattern in the file name (*.lua, or **/*.lua for subfolders); absolute, or relative to "
                        "lua/mcpagent_tmp/, then the mounted folder (lua_mount) and the game folder. Result: {files: [{file, group, "
                        "cases: [{name, status: pass|fail|error|skip, message?, line? (of the failing assertion), traceback?, output?, "
                        "duration_ms}], totals {pass, fail, error, skip}, error?, setup_error?, filtered?}], totals, ok}. "
                        "Big runs: `only_failures` lists only the fail/error cases (each file keeps its totals); `fields: [\"totals\", \"ok\"]` "
                        "returns just the verdict. A result too big for your client may be saved to a file: read the path it gives. "
                        "`case` runs only the cases whose name contains it. What a failing case prints (print, MsgN) comes back as "
                        "`output` (cut at 4 KB; `output`: all or none). "
                        "The run is one call on the main thread (the game is frozen meanwhile; for multi-frame tests use lua_job), so "
                        "every file runs in the same frame: Entity:Remove() only takes effect next frame, a removed entity is still "
                        "valid for the next file. frames_between_files (default 0) waits that many frames between files. Calls sent "
                        "together are queued and run one after the other; Lua globals, entities and convars persist between calls. "
                        "`realm: \"both\"` runs the files in the server, then the client (each file says its realm; for sh_ files). "
                        "The `budget` (default 10 s per step) installs a count hook, which makes LuaJIT interpret: code under it, "
                        "and helpers it calls, run several times slower (up to 40x measured) and are not compiled. Never time "
                        "anything under a budget: give that case `timing = true` (or timeout = 0), or pass budget 0 for the whole "
                        "run (no time limit then: a loop freezes the game until timeout_s). "
                        "Full format: resource mcpagent://tests-format. Example: {\"realm\": \"server\", \"path\": \"tests/*.lua\", "
                        "\"only_failures\": true}.",
                        json{{"properties",
                              {{"realm", json{{"type", "string"}, {"enum", json::array({"client", "server", "both"})},
                                              {"description", "Default server. both: server then client (shared files)."}}},
                               {"path", {{"type", "string"}, {"minLength", 1}, {"description", "File or pattern, absolute or relative to lua/mcpagent_tmp/ (then the mounted folder, the game folder)."}}},
                               {"budget", {{"type", "number"}, {"minimum", 0}, {"maximum", 120}, {"description", "Time limit per step (load, beforeAll, each case...) in seconds (default 10). 0 = none and no hook (for timing runs)."}}},
                               {"timeout_s", {{"type", "number"}, {"minimum", 1}, {"maximum", 3600}, {"description", "How long to wait for the game per call (default: budget x (20 per file + 5), 60 s without budget)."}}},
                               {"case", {{"type", "string"}, {"minLength", 1}, {"description", "Run only the cases whose name contains this text (case-insensitive)."}}},
                               {"only_failures", {{"type", "boolean"}, {"description", "List only the fail and error cases (totals still count all)."}}},
                               {"output", {{"type", "string"}, {"enum", json::array({"failures", "all", "none"})}, {"description", "Captured print/MsgN of a case: failing cases only (default), all cases, or none."}}},
                               {"frames_between_files", {{"type", "integer"}, {"minimum", 0}, {"maximum", 60}, {"description", "Wait this many frames between files (default 0): lets entity removals take effect."}}},
                               {"fields", {{"type", "array"}, {"items", {{"type", "string"}}}, {"maxItems", 10}, {"description", "Keep only these top-level keys of the result (e.g. [\"totals\", \"ok\"])."}}}}},
                             {"required", {"path"}}},
                        json{{"destructiveHint", true}}, tests_run_tool});
}

}  // namespace tools

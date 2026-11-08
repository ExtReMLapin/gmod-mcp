// Tools: status, lua_exec, lua_exec_file, lua_inspect, lua_file_*.
#include <algorithm>
#include <filesystem>

#include "agent.h"
#include "console.h"
#include "coordinator.h"
#include "jobs.h"
#include "mcp.h"
#include "platform/platform.h"
#include "realms.h"
#include "tools.h"
#include "util.h"

namespace tools {

json ok(json result) { return json{{"ok", true}, {"result", std::move(result)}}; }

json realm_prop(const std::string &desc, bool with_menu) {
    json e = with_menu ? json::array({"menu", "client", "server"}) : json::array({"client", "server"});
    return json{{"type", "string"}, {"enum", e}, {"description", desc}};
}

int realm_of(const json &args, const char *key, int def) {
    if (!args.contains(key)) return def;
    return realms::parse(args[key].get<std::string>());
}

json rt_realm(const json &args, const char *key, int def, const std::string &tool, const json &payload, double timeout_s) {
    int r = realm_of(args, key, def);
    if (r < 0) return agent::err("invalid_params", std::string(key) + ": unknown realm");
    return agent::rt(r, tool, payload, timeout_s);
}

static bool safe_rel(const std::string &name) {
    if (name.empty() || name.size() > 200) return false;
    if (name[0] == '/' || name[0] == '\\' || name.find(':') != std::string::npos) return false;
    for (char c : name)
        if (!(std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.' || c == '/')) return false;
    std::string s = "/" + name + "/";
    if (s.find("/../") != std::string::npos || s.find("/./") != std::string::npos || s.find("//") != std::string::npos) return false;
    // Windows reserved device names (con.lua, nul, com1...) in any path component: writing to them can block.
    static const char *reserved[] = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7",
                                     "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    size_t start = 0;
    while (start <= name.size()) {
        size_t end = name.find('/', start);
        if (end == std::string::npos) end = name.size();
        std::string comp = util::to_lower(name.substr(start, end - start));
        std::string stem = comp.substr(0, comp.find('.'));
        for (const char *r : reserved)
            if (stem == r) return false;
        if (!comp.empty() && (comp.back() == '.' || comp.back() == ' ')) return false;
        start = end + 1;
    }
    return true;
}

std::string tmp_path(const std::string &name, std::string &err) {
    if (!safe_rel(name)) {
        err = "invalid name: relative to mcpagent_tmp, characters [A-Za-z0-9_.-/], no '..'";
        return "";
    }
    return agent::config().tmp_dir + "/" + name;
}

static std::string normalize(std::string p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    return p;
}

// Chunk name for a file: path relative to the game folder when inside it (e.g. @lua/mcpagent_tmp/x.lua).
static std::string chunk_for(const std::string &abs) {
    std::string na = normalize(abs), game = normalize(agent::config().game_dir) + "/";
    if (util::to_lower(na).rfind(util::to_lower(game), 0) == 0) return "@" + na.substr(game.size());
    return "@" + na;
}

bool resolve_lua_file(const std::string &path, std::string &abs, std::string &chunkname, std::string &err) {
    std::filesystem::path p = std::filesystem::u8path(path);
    if (p.is_absolute()) {
        abs = path;
    } else {
        abs = tmp_path(path, err);
        if (abs.empty()) return false;
    }
    if (!plat::file_exists(abs)) {
        err = "file not found: " + abs;
        return false;
    }
    chunkname = chunk_for(abs);
    return true;
}

static json status_tool(const json &args) {
    const auto &c = agent::config();
    double since = jobs::seconds_since_last_frame();
    bool stalled = since > 5.0;
    json realms_j = json::object();
    for (int r = 0; r < realms::COUNT; ++r) {
        if (c.mode == agent::Mode::Dedicated && r != realms::SERVER) continue;
        realms_j[realms::name(r)] = json{{"available", agent::realm_present(r)},
                                         {"runtime", agent::realm_injected(r)},
                                         {"generation", agent::generation(r)}};
    }
    json s{{"instance", c.instance_id},
           {"mode", agent::mode_name()},
           {"agent_version", c.version},
           {"platform", plat::platform_tag()},
           {"pid", plat::process_id()},
           {"port", c.port},
           {"coordinator", coordinator::is_coordinator()},
           {"uptime_s", console::uptime()},
           {"frames", jobs::frame_count()},
           {"last_tick_age_s", since},
           {"main_thread_stalled", stalled},
           {"perf", jobs::frame_stats()},
           {"realms", realms_j},
           {"client_lock", agent::lock_status()},
           {"console_capture", console::method()},
           {"tmp_dir", c.tmp_dir},
           {"game_dir", c.game_dir},
           {"pending_jobs", jobs::pending()}};
    if (stalled) {
        s["warning"] = "the main thread has not run for " + std::to_string((int)since) +
                       " s (long load, freeze or infinite loop). game_terminate remains available.";
        return ok(s);
    }
    double to = args.value("timeout_s", 3.0);
    int host = c.mode == agent::Mode::Client ? realms::MENU : realms::SERVER;
    json h = agent::rt(host, "status", json::object(), to);
    if (h.value("ok", false)) s[c.mode == agent::Mode::Client ? "menu" : "server"] = h["result"];
    else s["host_error"] = h["error"];
    if (c.mode == agent::Mode::Client) {
        if (agent::realm_present(realms::SERVER)) {
            json sv = agent::rt(realms::SERVER, "status", json::object(), to);
            if (sv.value("ok", false)) s["server"] = sv["result"];
        }
        if (agent::realm_present(realms::CLIENT) && agent::lock_status().value("client_allowed", false)) {
            json cl = agent::rt(realms::CLIENT, "status", json::object(), to);
            if (cl.value("ok", false)) s["client"] = cl["result"];
        }
    }
    // Convenience summary.
    json src = s.contains("server") ? s["server"] : (s.contains("client") ? s["client"] : json());
    if (!src.is_null()) {
        s["map"] = src.value("map", "");
        s["gamemode"] = src.value("gamemode", "");
        s["maxplayers"] = src.value("maxplayers", 0);
    } else {
        s["map"] = nullptr;
        if (s.contains("menu")) {
            s["gamemode"] = s["menu"].value("gamemode", json());
            s["maxplayers"] = s["menu"].value("maxplayers", json());
        }
    }
    if (s.contains("menu")) {
        s["gmod_version"] = s["menu"].value("version", json());
        s["gmod_version_str"] = s["menu"].value("version_str", json());
        s["branch"] = s["menu"].value("branch", json());
        s["loading"] = s["menu"].value("loading", false);
    }
    return ok(s);
}

static json exec_payload(const json &args, const std::string &code, const std::string &chunk) {
    json p{{"code", code}, {"chunkname", chunk}};
    for (const char *k : {"budget", "depth", "limit"})
        if (args.contains(k)) p[k] = args[k];
    return p;
}

static double exec_timeout(const json &args) { return args.value("budget", 5.0) + 10.0; }

static json lua_exec_tool(const json &args) {
    return rt_realm(args, "realm", realms::MENU, "lua_exec", exec_payload(args, args["code"], "=mcpagent:lua_exec"), exec_timeout(args));
}

static json lua_exec_file_tool(const json &args) {
    std::string abs, chunk, e;
    if (!resolve_lua_file(args["path"], abs, chunk, e)) return agent::err("not_found", e);
    std::string code;
    if (!plat::read_file(abs, code)) return agent::err("io_error", "cannot read: " + abs);
    json r = rt_realm(args, "realm", realms::MENU, "lua_exec", exec_payload(args, code, chunk), exec_timeout(args));
    if (r.value("ok", false)) r["result"]["file"] = abs;
    return r;
}

static json lua_check_tool(const json &args) {
    constexpr size_t kMaxFiles = 2000, kMaxBytes = 8u << 20;
    bool has_code = args.contains("code"), has_path = args.contains("path");
    if (has_code == has_path) return agent::err("invalid_params", "pass exactly one of code or path");
    json items = json::array();
    std::string abs;
    if (has_code) {
        items.push_back(json{{"file", "code"}, {"chunk", "=mcpagent:lua_check"}, {"code", args["code"]}});
    } else {
        std::string path = args["path"], e;
        if (std::filesystem::u8path(path).is_absolute()) abs = path;
        else if ((abs = tmp_path(path, e)).empty()) return agent::err("invalid_params", e);
        std::error_code ec;
        auto fp = std::filesystem::u8path(abs);
        std::vector<std::string> files;
        if (std::filesystem::is_directory(fp, ec)) {
            std::string base = normalize(abs);
            while (base.size() > 1 && base.back() == '/') base.pop_back();
            for (const auto &rel : plat::list_files(abs, true)) {
                std::string l = util::to_lower(rel);
                if (l.size() > 4 && l.compare(l.size() - 4, 4, ".lua") == 0) files.push_back(base + "/" + rel);
            }
            std::sort(files.begin(), files.end());
            if (files.empty()) return agent::err("not_found", "no .lua file under: " + abs);
        } else if (std::filesystem::is_regular_file(fp, ec)) {
            files.push_back(abs);
        } else {
            return agent::err("not_found", "file not found: " + abs);
        }
        if (files.size() > kMaxFiles)
            return agent::err("invalid_params", std::to_string(files.size()) + " .lua files, the limit is " + std::to_string(kMaxFiles) +
                                                    ": check a subfolder");
        size_t total = 0;
        for (const auto &f : files) {
            std::string code;
            if (!plat::read_file(f, code)) return agent::err("io_error", "cannot read: " + f);
            if ((total += code.size()) > kMaxBytes) return agent::err("invalid_params", "more than 8 MB of Lua: check a subfolder");
            std::string chunk = chunk_for(f);
            items.push_back(json{{"file", chunk.substr(1)}, {"chunk", chunk}, {"code", std::move(code)}});
        }
    }
    json p{{"items", std::move(items)}, {"globals", args.value("globals", true)}, {"limit", args.value("limit", 200)}};
    json r = rt_realm(args, "realm", realms::MENU, "lua_check", p, 30.0);
    if (has_path && r.value("ok", false)) r["result"]["path"] = abs;
    return r;
}

static json lua_inspect_tool(const json &args) {
    json p{{"expr", args["expr"]}, {"depth", args.value("depth", 1)}, {"limit", args.value("limit", 100)}};
    return rt_realm(args, "realm", realms::MENU, "lua_inspect", p);
}

static json file_write(const json &args) {
    std::string e, p = tmp_path(args["name"], e);
    if (p.empty()) return agent::err("invalid_params", e);
    std::filesystem::path fp = std::filesystem::u8path(p);
    plat::make_dirs(fp.parent_path().u8string());
    std::string content = args["content"];
    if (!plat::write_file(p, content)) return agent::err("io_error", "cannot write: " + p);
    return ok(json{{"path", p}, {"bytes", content.size()}, {"exec_path", args["name"]}});
}

static json file_read(const json &args) {
    std::string e, p = tmp_path(args["name"], e);
    if (p.empty()) return agent::err("invalid_params", e);
    std::string content;
    if (!plat::read_file(p, content)) return agent::err("not_found", "file not found: " + p);
    return ok(json{{"path", p}, {"content", content}});
}

static json file_list(const json &) {
    auto files = plat::list_files(agent::config().tmp_dir, true);
    std::sort(files.begin(), files.end());
    return ok(json{{"dir", agent::config().tmp_dir}, {"files", files}});
}

static json file_delete(const json &args) {
    std::string e, p = tmp_path(args["name"], e);
    if (p.empty()) return agent::err("invalid_params", e);
    if (!plat::remove_file(p)) return agent::err("not_found", "file not found: " + p);
    return ok(json{{"deleted", p}});
}

void register_core() {
    json RO = {{"readOnlyHint", true}};
    json realm_exec = realm_prop("Target Lua state: 'menu' (always present), 'client' or 'server' (after map_load). Default: menu.");

    mcp::register_tool({"status", "Instance status",
                        "GMod version, branch, platform, map, gamemode, maxplayers, available realms (and injection generation), "
                        "client lock, fps/frametime, uptime, age of the main thread's last tick (main_thread_stalled if > 5 s), "
                        "Lua temporary directory (tmp_dir, absolute path). Call this first. Works even if the game is frozen.",
                        json{{"properties", {{"timeout_s", {{"type", "number"}, {"minimum", 0.1}, {"maximum", 30},
                                                            {"description", "Max wait for Lua info (default 3)."}}}}}},
                        RO, status_tool});

    json exec_props = {{"realm", realm_exec},
                       {"budget", {{"type", "number"}, {"minimum", 0.05}, {"maximum", 120},
                                   {"description", "Time budget in seconds (default 5). Beyond it, execution is interrupted (budget_exceeded)."}}},
                       {"depth", {{"type", "integer"}, {"minimum", 0}, {"maximum", 12}, {"description", "Table serialization depth (default 4)."}}},
                       {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10000}, {"description", "Max number of elements per table (default 200)."}}}};
    json exec_code = exec_props;
    exec_code["code"] = {{"type", "string"}, {"description", "Lua code. Use `return` to return values (several allowed)."}};
    mcp::register_tool({"lua_exec", "Execute Lua",
                        "Executes Lua code in a realm under xpcall + debug.traceback, with a time budget (debug.sethook; JIT disabled "
                        "for the chunk). Returns all serialized return values (values, n), captured print/Msg/MsgN/MsgC "
                        "(output) and the duration. Non-JSON types: {__type: Vector|Angle|Color|Entity|function|cycle|...}. "
                        "Errors: lua_syntax_error, lua_error (with traceback), budget_exceeded, realm_unavailable, client_locked.",
                        json{{"properties", exec_code}, {"required", {"code"}}}, json{{"destructiveHint", true}}, lua_exec_tool});

    json exec_file = exec_props;
    exec_file["path"] = {{"type", "string"},
                         {"description", "Path relative to the lua/mcpagent_tmp/ temporary directory (e.g. 'test.lua'), or an absolute path on the game's disk."}};
    mcp::register_tool({"lua_exec_file", "Execute a Lua file",
                        "Like lua_exec but reads the code from a file on the game's disk. The chunk is named after the real path "
                        "(e.g. @lua/mcpagent_tmp/test.lua) for actionable errors and tracebacks. First write the file "
                        "directly into tmp_dir (see status) or via lua_file_write.",
                        json{{"properties", exec_file}, {"required", {"path"}}}, json{{"destructiveHint", true}}, lua_exec_file_tool});

    mcp::register_tool(
        {"lua_check", "Check Lua without running it",
         "Compiles Lua with the game's own parser (GLua syntax: !=, &&, ||, !, //, /* */, continue) and runs nothing. "
         "`code`, or `path`: a file or a whole folder (every .lua under it, e.g. an addon), relative to lua/mcpagent_tmp/ or "
         "absolute. Returns ok, errors [{file, line, message}] (one per file: the parser stops at the first error), and from "
         "the bytecode: unknown_globals (globals read but neither defined in the checked files nor present in the realm: "
         "typos, missing includes, wrong realm) and globals_written (every global assignment, e.g. a forgotten `local`). "
         "Unknown globals depend on the realm: pick the one the code runs in ('client'/'server' need a map; 'menu' lacks "
         "most game functions). ENT, SWEP, GM, EFFECT and TOOL are never reported. Errors: realm_unavailable, not_found.",
         json{{"properties",
               {{"realm", realm_prop("Lua state whose parser and globals are used: 'menu' (default, always present), 'client' or 'server'.")},
                {"code", {{"type", "string"}, {"description", "Lua code to check (instead of path)."}}},
                {"path", {{"type", "string"}, {"description", "File or folder to check (instead of code): relative to lua/mcpagent_tmp/, or an absolute path on the game's disk."}}},
                {"globals", {{"type", "boolean"}, {"description", "Also list unknown and written globals (default true)."}}},
                {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 5000}, {"description", "Max entries per list (default 200)."}}}}}},
         RO, lua_check_tool});

    mcp::register_tool({"lua_inspect", "Inspect a Lua value",
                        "Evaluates an expression (expected to be side-effect free) and describes the value: type, value serialized to the given depth, "
                        "keys (with types) up to `limit`, metatable (name, keys, __index methods). Depth 0 = summary only.",
                        json{{"properties",
                              {{"realm", realm_exec},
                               {"expr", {{"type", "string"}, {"description", "Lua expression, e.g. 'hook.GetTable()' or 'FindMetaTable(\"Entity\")'."}}},
                               {"depth", {{"type", "integer"}, {"minimum", 0}, {"maximum", 8}, {"description", "Depth (default 1)."}}},
                               {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 5000}, {"description", "Max number of keys (default 100)."}}}}},
                             {"required", {"expr"}}},
                        RO, lua_inspect_tool});

    json name_prop = {{"type", "string"}, {"description", "Name relative to the lua/mcpagent_tmp/ directory (e.g. 'scan.lua', 'lib/util.lua')."}};
    mcp::register_tool({"lua_file_write", "Write a temporary Lua file",
                        "Writes a file into the game's lua/mcpagent_tmp/ temporary directory (creates subdirectories). Useful when the MCP "
                        "client does not share the game's disk. Then: lua_exec_file {path: name}.",
                        json{{"properties", {{"name", name_prop}, {"content", {{"type", "string"}, {"description", "File content."}}}}},
                             {"required", {"name", "content"}}},
                        json{{"destructiveHint", false}, {"idempotentHint", true}}, file_write});
    mcp::register_tool({"lua_file_read", "Read a temporary Lua file", "Reads a file from the lua/mcpagent_tmp/ directory.",
                        json{{"properties", {{"name", name_prop}}}, {"required", {"name"}}}, RO, file_read});
    mcp::register_tool({"lua_file_list", "List temporary Lua files",
                        "Recursively lists the lua/mcpagent_tmp/ directory (absolute path in `dir`).", json::object(), RO, file_list});
    mcp::register_tool({"lua_file_delete", "Delete a temporary Lua file", "Deletes a file from the lua/mcpagent_tmp/ directory.",
                        json{{"properties", {{"name", name_prop}}}, {"required", {"name"}}}, json{{"destructiveHint", true}}, file_delete});
}

void register_all() {
    static bool done = false;
    if (done) return;
    done = true;
    register_core();
    register_session();
    register_console();
    register_world();
    register_render();
    register_docs();
    register_reports();
    register_lua_tools();
    register_jobs();
    register_instances();
    // Long-running tools executable as MCP tasks (declared after all tools are registered).
    for (const char *n : {"events_wait", "reports_read"}) mcp::set_task_support(n, "optional");
}

}  // namespace tools

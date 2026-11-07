// Tools: console_exec, console_read, errors_since, errors_clear, convar_get, convar_set, concommand_list.
#include <chrono>
#include <regex>
#include <set>

#include "agent.h"
#include "console.h"
#include "events.h"
#include "jobs.h"
#include "mcp.h"
#include "platform/platform.h"
#include "realms.h"
#include "netaddr.h"
#include "tools.h"
#include "util.h"

namespace tools {

static json lines_json(const std::vector<console::Line> &lines) {
    json arr = json::array();
    for (const auto &l : lines) arr.push_back(json{{"seq", l.seq}, {"t", l.time}, {"level", l.level}, {"text", l.text}});
    return arr;
}

static void wait_frames(uint64_t n, double max_s) {
    uint64_t start = jobs::frame_count();
    auto t0 = std::chrono::steady_clock::now();
    while (jobs::frame_count() < start + n &&
           std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < max_s)
        plat::sleep_ms(2);
}

static int default_realm() { return agent::config().mode == agent::Mode::Client ? realms::MENU : realms::SERVER; }

// Non-negotiable rule: never connect to a remote server. The engine splits a command line on ';' and newlines
// (outside quotes), so every sub-command is checked, not only the first one.
static std::string forbidden_one(const std::string &cmd) {
    auto parts = util::split_args(cmd);
    if (parts.empty()) return "";
    std::string name = util::to_lower(parts[0]);
    if (name == "retry") return "retry refused: the reconnection target cannot be verified";
    if (name == "connect" || name == "redirect" || name == "connect_lobby") {
        if (parts.size() < 2 || !netaddr::is_this_machine(parts[1]))
            return name + " refused: only servers on this machine (loopback or one of its own interface addresses) are allowed";
    }
    return "";
}

static std::string forbidden_network_cmd(const std::string &line) {
    std::string cur;
    bool inq = false;
    for (char c : line + ";") {
        if (c == '"') inq = !inq;
        if (!inq && (c == ';' || c == '\n' || c == '\r')) {
            std::string why = forbidden_one(cur);
            if (!why.empty()) return why;
            cur.clear();
        } else {
            cur += c;
        }
    }
    return "";
}

static json console_exec_tool(const json &args) {
    int r = realm_of(args, "realm", default_realm());
    if (r < 0) return agent::err("invalid_params", "unknown realm");
    std::string cmd = args["cmd"];
    std::string why = forbidden_network_cmd(cmd);
    if (!why.empty()) return agent::err("forbidden", why);
    int frames = args.value("wait_frames", 5);
    uint64_t c0 = console::cursor_now();
    uint64_t ev0 = events::cursor_now();
    auto parts = util::split_args(cmd);
    bool is_connect = !parts.empty() && util::to_lower(parts[0]) == "connect" && agent::config().mode == agent::Mode::Client;
    json res = agent::rt(r, "console_exec", json{{"cmd", cmd}});
    if (!res.value("ok", false)) return res;
    json out = json{{"cmd", cmd}, {"realm", realms::name(r)}, {"via", realms::name(r)}};
    if (res["result"].value("blocked", false)) {
        // Realm's Lua blacklist: fall back to the engine (IVEngineClient::ClientCmd), via the menu state.
        out["blocked_in_lua"] = true;
        if (agent::config().mode != agent::Mode::Client)
            return agent::err("blocked", "command blocked from Lua and no engine path in srcds mode: " + cmd);
        c0 = console::cursor_now();
        res = agent::rt(realms::MENU, "engine_cmd", json{{"cmd", cmd}});
        if (!res.value("ok", false)) return res;
        out["via"] = "engine";
    }
    wait_frames(frames, 5.0);
    std::string e;
    // connect: wait for the outcome and report it, so the agent knows at once (a failure also opens a native game dialog
    // that the agent cannot click).
    if (is_connect) {
        double timeout = args.value("connect_timeout_s", 20.0);
        json w = events::wait({"realm_ready", "disconnect_reason", "connection_failed"}, ev0, timeout, 20);
        json result{{"status", "pending"}};
        for (const auto &ev : w["events"]) {
            std::string t = ev["type"];
            if (t == "realm_ready" && ev["data"].value("realm", "") == "client") {
                result = json{{"status", "connected"}, {"map", ev["data"].value("map", "")}};
                break;
            }
            if (t == "disconnect_reason" || t == "connection_failed") {
                std::string reason = ev["data"].value("reason", ev["data"].value("message", ""));
                result = json{{"status", "failed"},
                              {"reason", reason},
                              {"note", "the game shows an error dialog (Copy/OK) that cannot be closed from here: ask the user to "
                                       "click OK, or fix the address and retry"}};
                break;
            }
        }
        if (result["status"] == "pending")
            result["note"] = "no outcome within " + std::to_string((int)timeout) +
                             " s (still connecting or downloading content?); use events_wait {types:[\"realm_ready\",\"disconnect_reason\"]}";
        out["connect_result"] = result;
    }
    auto rd = console::read(c0, 2000, "", e);
    out["output"] = lines_json(rd.lines);
    out["next_cursor"] = rd.next_cursor;
    return ok(out);
}

static json console_read_tool(const json &args) {
    size_t limit = args.value("limit", 200);
    std::string re = args.value("regex", "");
    std::string e;
    console::ReadResult r;
    if (args.contains("cursor")) {
        r = console::read(args["cursor"].get<uint64_t>(), limit, re, e);
    } else {
        // No cursor: the last `limit` lines (matching the filter) of the whole buffer.
        r = console::read(0, SIZE_MAX, re, e);
        if (r.lines.size() > limit) r.lines.erase(r.lines.begin(), r.lines.end() - (std::ptrdiff_t)limit);
        r.lost = 0;
    }
    if (!e.empty()) return agent::err("invalid_params", e);
    return ok(json{{"lines", lines_json(r.lines)}, {"next_cursor", r.next_cursor}, {"lost", r.lost}, {"oldest", r.oldest},
                   {"capture", console::method()}});
}

static json errors_since_tool(const json &args) {
    json r = console::errors_since(args.value("cursor", (uint64_t)0), args.value("limit", 100));
    if (args.contains("realm")) {
        json f = json::array();
        for (const auto &e : r["errors"])
            if (e.value("realm", "") == args["realm"].get<std::string>()) f.push_back(e);
        r["errors"] = f;
    }
    return ok(r);
}

static json convar_set_tool(const json &args) {
    int r = realm_of(args, "realm", default_realm());
    if (r < 0) return agent::err("invalid_params", "unknown realm");
    std::string value = args["value"].is_string() ? args["value"].get<std::string>() : args["value"].dump();
    // An FCVAR_ARCHIVE convar is written back to the user's cfg/config.cfg when the game closes: refused by default.
    json before = agent::rt(r, "convar_get", json{{"name", args["name"]}});
    if (!before.value("ok", false)) return before;
    bool archived = false;
    for (const auto &f : before["result"].value("flag_names", json::array())) archived = archived || f == "ARCHIVE";
    if (archived && !args.value("persist", false))
        return agent::err("archived", "FCVAR_ARCHIVE convar: changing it would write to the user's cfg/config.cfg. Pass persist=true to accept.",
                          json{{"current", before["result"].value("value", "")}});
    json s = agent::rt(r, "convar_set", json{{"name", args["name"]}, {"value", value}});
    if (!s.value("ok", false)) return s;
    wait_frames(3, 3.0);
    json g = agent::rt(r, "convar_get", json{{"name", args["name"]}});
    if (!g.value("ok", false)) return g;
    std::string now = g["result"].value("value", "");
    if (now != value) {
        // Numeric normalization (e.g. "1" vs "1.0").
        bool same = false;
        try {
            same = std::stod(now) == std::stod(value);
        } catch (...) {
        }
        if (!same)
            return agent::err("set_refused", "the convar did not take the requested value (cheat without sv_cheats, server convar, or blocked command)",
                              json{{"requested", value}, {"current", now}, {"flags", g["result"]["flag_names"]}});
    }
    json out = g["result"];
    out["previous"] = s["result"]["previous"];
    return ok(out);
}

static json concommand_list_tool(const json &args) {
    int r = realm_of(args, "realm", default_realm());
    if (r < 0) return agent::err("invalid_params", "unknown realm");
    std::string filter = args.value("filter", "");
    json lua = agent::rt(r, "concommand_list", json{{"filter", filter}});
    if (!lua.value("ok", false)) return lua;
    json out = lua["result"];
    out["realm"] = realms::name(r);
    if (args.value("engine", true) && !filter.empty()) {
        // Engine commands and convars: parsed output of "find <filter>".
        uint64_t c0 = console::cursor_now();
        json res = agent::rt(default_realm(), "console_exec", json{{"cmd", "find " + filter}});
        if (res.value("ok", false)) {
            wait_frames(5, 5.0);
            std::string e;
            auto rd = console::read(c0, 5000, "", e);
            static const std::regex cvar_re("^\"([^\"]+)\" = \"([^\"]*)\"");
            static const std::regex cmd_re("^\"([^\"]+)\"\\s*$");
            std::set<std::string> cmds, cvars;
            for (const auto &l : rd.lines) {
                std::smatch m;
                if (std::regex_search(l.text, m, cvar_re)) cvars.insert(m[1].str());
                else if (std::regex_search(l.text, m, cmd_re)) cmds.insert(m[1].str());
            }
            out["engine_commands"] = cmds;
            out["engine_convars"] = cvars;
        }
    }
    return ok(out);
}

void register_console() {
    json RO = {{"readOnlyHint", true}};
    json realm_p = realm_prop("Execution realm (default: menu in client mode, server on srcds).");
    mcp::register_tool({"console_exec", "Run a console command",
                        "Runs a console command from a realm (menu: RunConsoleCommand; server: game.ConsoleCommand; client: "
                        "RunConsoleCommand, with automatic fallback to the menu if the command is blocked client-side). Waits "
                        "a few frames then returns the console lines produced (multi-line output included).",
                        json{{"properties", {{"cmd", {{"type", "string"}, {"minLength", 1}, {"description", "Command line, e.g. 'echo hello' or 'sv_gravity 600'."}}},
                                             {"realm", realm_p},
                                             {"wait_frames", {{"type", "integer"}, {"minimum", 0}, {"maximum", 600}, {"description", "Frames to wait before reading the output (default 5)."}}},
                                             {"connect_timeout_s", {{"type", "number"}, {"minimum", 1}, {"maximum", 300}, {"description", "For `connect`: how long to wait for the outcome, reported in connect_result (default 20)."}}}}},
                             {"required", {"cmd"}}},
                        json{{"destructiveHint", true}}, console_exec_tool});
    mcp::register_tool({"console_read", "Read the console",
                        "Incremental console read (ring buffer, monotonic cursor). Without `cursor`: the last `limit` lines. "
                        "Reuse next_cursor. `lost` > 0 = lines lost to buffer overflow. Regex filter (ECMAScript, case-insensitive).",
                        json{{"properties", {{"cursor", {{"type", "integer"}, {"minimum", 0}, {"description", "Cursor (previous next_cursor)."}}},
                                             {"regex", {{"type", "string"}, {"description", "Optional filter."}}},
                                             {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10000}, {"description", "Default 200."}}}}}},
                        RO, console_read_tool});
    mcp::register_tool({"console_buffer", "Console buffer capacity",
                        "Reads or sets the capacity (in lines) of the console ring buffer (default 20000).",
                        json{{"properties", {{"capacity", {{"type", "integer"}, {"minimum", 16}, {"maximum", 1000000}}}}}},
                        json{{"idempotentHint", true}, {"destructiveHint", false}}, [](const json &a) {
                            if (a.contains("capacity")) console::set_capacity(a["capacity"].get<size_t>());
                            return ok(json{{"capacity", console::capacity()}, {"cursor", console::cursor_now()}});
                        }});
    mcp::register_tool({"errors_since", "Read Lua errors",
                        "Lua errors from all realms (menu state OnLuaError hook; on srcds: console parsing) with realm, "
                        "message and stack. Monotonic cursor (0 = from the start of the buffer).",
                        json{{"properties", {{"cursor", {{"type", "integer"}, {"minimum", 0}, {"description", "Default 0."}}},
                                             {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 2000}, {"description", "Default 100."}}},
                                             {"realm", realm_prop("Optional realm filter.")}}}},
                        RO, errors_since_tool});
    mcp::register_tool({"errors_clear", "Clear errors", "Clears the error buffer (the cursor keeps increasing).", json::object(),
                        json{{"destructiveHint", true}, {"idempotentHint", true}},
                        [](const json &) {
                            console::errors_clear();
                            return ok(json{{"cleared", true}, {"next_cursor", console::errors_cursor()}});
                        }});
    mcp::register_tool({"convar_get", "Read a convar",
                        "Value, default, help text, flags (decoded), min/max of a convar. FCVAR_PROTECTED convars have their value masked.",
                        json{{"properties", {{"name", {{"type", "string"}, {"minLength", 1}}}, {"realm", realm_p}}}, {"required", {"name"}}},
                        RO, [](const json &a) { return rt_realm(a, "realm", default_realm(), "convar_get", json{{"name", a["name"]}}); }});
    mcp::register_tool({"convar_set", "Set a convar",
                        "Sets a convar then re-reads its value after a few frames. Errors: not_found, protected, archived (FCVAR_ARCHIVE without persist=true), set_refused "
                        "(value not applied: cheat, restriction...).",
                        json{{"properties", {{"name", {{"type", "string"}, {"minLength", 1}}},
                                             {"value", {{"type", {"string", "number", "boolean"}}, {"description", "New value."}}},
                                             {"realm", realm_p},
                                             {"persist", {{"type", "boolean"}, {"description", "Allow changing an FCVAR_ARCHIVE convar (persisted in config.cfg). Default false."}}}}},
                             {"required", {"name", "value"}}},
                        json{{"destructiveHint", false}, {"idempotentHint", true}}, convar_set_tool});
    mcp::register_tool({"concommand_list", "List commands",
                        "Lua commands (the realm's concommand.GetTable) and, if a filter is given, engine commands and convars "
                        "(output of 'find <filter>').",
                        json{{"properties", {{"filter", {{"type", "string"}, {"description", "Substring."}}},
                                             {"realm", realm_p},
                                             {"engine", {{"type", "boolean"}, {"description", "Include engine commands (default true, requires a filter)."}}}}}},
                        RO, concommand_list_tool});
}

}  // namespace tools

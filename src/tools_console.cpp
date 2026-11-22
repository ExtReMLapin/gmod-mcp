// Tools: console_exec, console_read, errors_since, errors_clear, convar_get, convar_set, concommand_list.
#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <vector>

#include "agent.h"
#include "cmdline.h"
#include "console.h"
#include "events.h"
#include "jobs.h"
#include "mcp.h"
#include "platform/platform.h"
#include "realms.h"
#include "netaddr.h"
#include "reqctx.h"
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

// Checks the very statements console_exec will run (cmdline::split_statements): a separate splitter here kept a quote
// open across a line break, so 'echo "a<newline>connect x' passed as one echo and ran as an echo plus a connect.
std::string forbidden_network_cmd(const std::string &line) {
    for (const auto &st : cmdline::split_statements(line)) {
        std::string why = forbidden_one(st);
        if (!why.empty()) return why;
    }
    return "";
}

// Sends one statement from a realm; a statement blocked from Lua goes through the engine (menu state). Returns the realm
// result or an error; `via` says which way.
static json send_statement(int r, const std::string &stmt, std::string &via) {
    json res = agent::rt(r, "console_exec", json{{"cmd", stmt}});
    via = realms::name(r);
    if (!res.value("ok", false) || !res["result"].value("blocked", false)) return res;
    // Realm's Lua blacklist: fall back to the engine (IVEngineClient::ClientCmd), via the menu state.
    if (agent::config().mode != agent::Mode::Client)
        return agent::err("blocked", "command blocked from Lua and no engine path in srcds mode: " + stmt);
    via = "engine";
    return agent::rt(realms::MENU, "engine_cmd", json{{"cmd", stmt}});
}

static json console_exec_tool(const json &args) {
    int r = realm_of(args, "realm", default_realm());
    if (r < 0) return agent::err("invalid_params", "unknown realm");
    std::string cmd = args["cmd"];
    std::string why = forbidden_network_cmd(cmd);
    if (!why.empty()) return agent::err("forbidden", why);
    // The engine runs "a 1; b 2" as two commands: so do we (sending it as one made "1; b 2" the value of a).
    std::vector<std::string> stmts = cmdline::split_statements(cmd);
    if (stmts.empty()) return agent::err("invalid_params", "empty command");
    int frames = args.value("wait_frames", 5);
    uint64_t c0 = console::cursor_now();
    uint64_t ev0 = events::cursor_now();
    bool client = agent::config().mode == agent::Mode::Client;
    // Any statement (not only the first) that is `connect`, next to others, is refused: "echo x; connect ..." used to be
    // sent as is, without leaving the current game first.
    if (client && cmdline::connect_not_alone(stmts)) return agent::err("invalid_params", "connect must be alone on its command line");
    bool is_connect = client && cmdline::command_name(stmts[0]) == "connect";
    // Connecting while this game still runs (or hosts) a map crashes the engine: leave the current game first.
    if (is_connect && (agent::realm_present(realms::SERVER) || agent::realm_present(realms::CLIENT))) {
        // Internal call: target this instance explicitly (otherwise the ambiguous_instance guard would refuse it).
        json d = mcp::call_tool("disconnect", json{{"instance", agent::config().instance_id}});
        if (d.value("isError", false)) return agent::err("busy", "could not leave the current game before connecting", d["structuredContent"]);
        c0 = console::cursor_now();
        ev0 = events::cursor_now();
    }
    json sent = json::array();
    std::string via_all;
    bool left_game = false;
    for (const auto &st : stmts) {
        std::string via;
        int sr = r;
        // Crash on killserver: run on a hosted map at low fps it crashes the game (cmdline::leave_game_first). Leave the
        // map first (disconnect waits until both realms are gone); killserver then only resets the network mode.
        if (client && cmdline::leave_game_first(st) &&
            (agent::realm_present(realms::SERVER) || agent::realm_present(realms::CLIENT))) {
            json d = mcp::call_tool("disconnect", json{{"instance", agent::config().instance_id}});
            if (d.value("isError", false)) {
                json details = d["structuredContent"];
                if (!sent.empty()) details["already_sent"] = sent;
                return agent::err("busy", "could not leave the current game before " + cmdline::command_name(st), details);
            }
            left_game = true;
        }
        if (left_game && sr != realms::MENU && !agent::realm_present(sr)) sr = realms::MENU;  // that realm is gone now
        json res = send_statement(sr, st, via);
        if (!res.value("ok", false)) {
            if (sent.empty()) return res;
            json e = res;  // earlier statements already ran: say which
            e["error"]["details"]["already_sent"] = sent;
            return e;
        }
        sent.push_back(json{{"cmd", st}, {"via", via}});
        via_all = via_all.empty() || via_all == via ? via : "mixed";
    }
    json out = json{{"cmd", cmd}, {"realm", realms::name(r)}, {"via", via_all}};
    if (stmts.size() > 1) out["statements"] = sent;
    if (via_all == "engine") out["blocked_in_lua"] = true;
    if (left_game) out["left_game_first"] = true;
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
    // The console is shared: keep gmod-mcp's own log lines out, and apply the caller's filter.
    std::unique_ptr<std::regex> rx;
    if (args.contains("regex")) {
        try {
            rx = std::make_unique<std::regex>(args["regex"].get<std::string>(), std::regex::ECMAScript | std::regex::icase);
        } catch (const std::exception &ex) {
            return agent::err("invalid_params", std::string("invalid regex: ") + ex.what());
        }
    }
    std::vector<console::Line> kept;
    std::vector<std::string> texts;
    for (const auto &l : rd.lines) {
        texts.push_back(l.text);
        if (cmdline::own_log_line(l.text)) continue;
        if (rx && !std::regex_search(l.text, *rx)) continue;
        kept.push_back(l);
    }
    out["output"] = lines_json(kept);
    out["next_cursor"] = rd.next_cursor;
    out["output_note"] = "console lines printed while the command ran; the console is shared, so other prints of the same "
                         "frames can appear (filter with `regex`)";
    // Unknown commands: the engine only prints a line, the call itself "worked".
    std::vector<std::string> unknown;
    for (const auto &u : cmdline::unknown_commands(texts))
        for (const auto &st : stmts)
            if (cmdline::command_name(st) == u && std::find(unknown.begin(), unknown.end(), u) == unknown.end()) unknown.push_back(u);
    if (!unknown.empty())
        return agent::err("unknown_command", "unknown command or convar: " + unknown[0] + (unknown.size() > 1 ? " (and others)" : "") +
                                                 " (typo, wrong realm, or the addon that defines it is not loaded)",
                          json{{"unknown", unknown}, {"statements", sent}, {"output", out["output"]}});
    // Convars: read the value back, so the caller knows whether it took (and warn when it lands in the user's config).
    json convars = json::object();
    for (const auto &st : stmts) {
        auto parts = util::split_args(st);
        if (parts.empty() || parts.size() > 2) continue;
        // Read it where it lives: a convar an addon creates in the client or server realm is not visible from the menu.
        json g;
        for (int rr : {r, (int)realms::CLIENT, (int)realms::SERVER}) {
            if (rr != r && (r != realms::MENU || !agent::realm_present(rr))) continue;
            g = agent::rt(rr, "convar_get", json{{"name", parts[0]}}, 5.0);
            if (g.value("ok", false)) break;
        }
        if (!g.value("ok", false)) continue;  // a command, or a protected convar
        json c{{"value", g["result"].value("value", json())}};
        if (g["result"].contains("number")) c["number"] = g["result"]["number"];
        if (parts.size() == 2) c["requested"] = parts[1];
        for (const auto &f : g["result"].value("flag_names", json::array()))
            if (f == "ARCHIVE")
                c["warning"] = "FCVAR_ARCHIVE: saved to cfg/config.cfg when the game closes, and shared by every instance "
                               "and the user's own game (convar_set refuses it unless persist: true)";
        convars[g["result"].value("name", parts[0])] = c;
    }
    if (!convars.empty()) out["convars"] = convars;
    return ok(out);
}

// console_wait: blocks until `count` console lines match `regex` (end of a long batch, a test's final line...).
static json console_wait_tool(const json &args) {
    std::string re = args["regex"];
    size_t count = args.value("count", (size_t)1);
    double timeout = args.value("timeout_s", 30.0);
    uint64_t cursor = args.contains("cursor") ? args["cursor"].get<uint64_t>() : console::cursor_now();
    auto t0 = std::chrono::steady_clock::now();
    json found = json::array();
    uint64_t lost = 0;
    double last_progress = -1;
    for (;;) {
        std::string e;
        auto rd = console::read(cursor, 10000, re, e);
        if (!e.empty()) return agent::err("invalid_params", e);
        lost += rd.lost;
        for (const auto &l : rd.lines) {
            if (found.size() < count) found.push_back(json{{"seq", l.seq}, {"t", l.time}, {"level", l.level}, {"text", l.text}});
        }
        cursor = rd.next_cursor;
        double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (found.size() >= count) {
            // next_cursor: just after the last line used, so a second wait does not miss lines printed meanwhile.
            uint64_t next = found.back()["seq"].get<uint64_t>() + 1;
            json out{{"matched", found.size()}, {"lines", found}, {"next_cursor", next}, {"waited_s", el}};
            if (lost) out["lost"] = lost;
            return ok(out);
        }
        if (el >= timeout)
            return agent::err("timeout", std::to_string(found.size()) + " of " + std::to_string(count) + " matching lines after " +
                                             std::to_string((int)timeout) + " s",
                              json{{"matched", found.size()}, {"lines", found}, {"next_cursor", cursor}, {"lost", lost}});
        if (reqctx::cancelled()) return agent::err("cancelled", "console_wait cancelled", json{{"matched", found.size()}});
        if (el - last_progress >= 1.0) {
            last_progress = el;
            reqctx::progress((double)found.size(), (double)count, std::to_string(found.size()) + "/" + std::to_string(count) + " lines");
        }
        plat::sleep_ms(50);
    }
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

// Issue #24: where an error was raised, as "file:line": the place the message starts with (what the engine puts in front of every
// Lua error), else the first stack frame. Backslashes read as slashes. "" when neither is known.
static std::string error_source(const json &e) {
    std::string msg = e.value("message", "");
    static const std::regex loc(R"(^\s*@?([^\s:][^:]*\.lua):(\d+):)", std::regex::ECMAScript);
    std::smatch m;
    std::string head = msg.substr(0, 400);
    std::string src;
    if (std::regex_search(head, m, loc)) {
        src = m[1].str() + ":" + m[2].str();
    } else if (e.contains("stack") && e["stack"].is_array() && !e["stack"].empty() && e["stack"][0].is_object()) {
        const json &f = e["stack"][0];
        if (f.contains("file") && f["file"].is_string()) {
            src = f["file"].get<std::string>();
            if (f.contains("line") && f["line"].is_number()) src += ":" + std::to_string(f["line"].get<long long>());
        }
    }
    std::replace(src.begin(), src.end(), '\\', '/');
    return src;
}

static json errors_since_tool(const json &args) {
    uint64_t cur_load = agent::map_load_seq();
    bool only_current = args.value("since_map_load", false);
    std::string realm = args.contains("realm") ? args["realm"].get<std::string>() : "";
    bool has_realm = args.contains("realm");
    // Issue #11: group_by "message" reads everything kept from the cursor (the store holds at most 2000), then `limit` caps the groups.
    bool grouped = args.value("group_by", "") == "message";
    size_t limit = args.value("limit", 100);
    // Issue #24: filters on the place and the text of the error (all given filters must hold).
    auto norm = [](std::string v) {
        std::replace(v.begin(), v.end(), '\\', '/');
        return util::to_lower(v);
    };
    std::string src_in = args.contains("source") ? norm(args["source"].get<std::string>()) : "";
    std::string src_out = args.contains("exclude_source") ? norm(args["exclude_source"].get<std::string>()) : "";
    bool has_src_in = args.contains("source"), has_src_out = args.contains("exclude_source");
    std::unique_ptr<std::regex> pat_in, pat_out;
    for (const char *k : {"pattern", "exclude_pattern"}) {
        if (!args.contains(k)) continue;
        try {
            auto rx = std::make_unique<std::regex>(args[k].get<std::string>(), std::regex::ECMAScript | std::regex::icase);
            (std::string(k) == "pattern" ? pat_in : pat_out) = std::move(rx);
        } catch (const std::exception &ex) {
            return agent::err("invalid_params", std::string(k) + ": invalid regex: " + ex.what());
        }
    }
    json r;
    try {
        // The filters apply while reading, so `limit` counts the errors kept and next_cursor stays exact.
        r = console::errors_since(args.value("cursor", (uint64_t)0), grouped ? (size_t)1 << 30 : limit, [&](const json &e) {
            if (has_realm && e.value("realm", "") != realm) return false;
            if (only_current && e.value("map_load", (uint64_t)0) != cur_load) return false;
            if (has_src_in || has_src_out) {
                std::string src = util::to_lower(error_source(e));
                if (has_src_in && src.compare(0, src_in.size(), src_in) != 0) return false;
                if (has_src_out && !src_out.empty() && src.compare(0, src_out.size(), src_out) == 0) return false;
            }
            if (pat_in || pat_out) {
                std::string msg = e.value("message", "");
                if (pat_in && !std::regex_search(msg, *pat_in)) return false;
                if (pat_out && std::regex_search(msg, *pat_out)) return false;
            }
            return true;
        });
    } catch (const std::regex_error &ex) {  // error_complexity / error_stack on a pathological pattern
        return agent::err("invalid_params", std::string("the pattern is too expensive to run: ") + ex.what());
    }
    json warnings = json::array();
    if (r.value("lost", (uint64_t)0) > 0) warnings.push_back(console::errors_lost_warning(r["lost"].get<uint64_t>()));
    if (grouped) {
        std::vector<json> groups;
        std::map<std::string, size_t> index;
        for (const auto &e : r["errors"]) {
            std::string msg = e.value("message", "");
            std::string src = error_source(e);
            std::string key = msg + "\n" + src;  // the same text raised from two places is two groups
            auto it = index.find(key);
            if (it == index.end()) {
                index[key] = groups.size();
                groups.push_back(json{{"message", msg}, {"count", 0}, {"first_seq", e.value("seq", 0)}, {"first_time", e.value("time", 0.0)},
                                      {"realms", json::array()}, {"stack", e.value("stack", json::array())}});
                it = index.find(key);
                if (!src.empty()) groups[it->second]["source"] = src;
                std::string hook = console::hook_of(msg);
                if (!hook.empty()) groups[it->second]["hook"] = hook;
                if (e.contains("addon")) groups[it->second]["addon"] = e["addon"];
                if (e.contains("addon_id")) groups[it->second]["addon_id"] = e["addon_id"];
            }
            json &g = groups[it->second];
            g["count"] = g["count"].get<int>() + 1;
            g["last_seq"] = e.value("seq", 0);
            g["last_time"] = e.value("time", 0.0);
            std::string m = agent::map_of_load(e.value("map_load", (uint64_t)0));
            if (!m.empty()) g["map"] = m;
            std::string realm_e = e.value("realm", "");
            bool seen = false;
            for (const auto &x : g["realms"]) seen = seen || x == realm_e;
            if (!seen) g["realms"].push_back(realm_e);
        }
        std::stable_sort(groups.begin(), groups.end(), [](const json &a, const json &b) { return a["count"].get<int>() > b["count"].get<int>(); });
        r.erase("errors");
        r["groups_total"] = groups.size();
        if (groups.size() > limit) groups.resize(limit);
        r["groups"] = groups;
        r["map_load"] = cur_load;
        r["map"] = agent::map_of_load(cur_load);
        if (!warnings.empty()) r["warnings"] = warnings;
        return ok(r);
    }
    // The plain list: when most of it is one message, say so (the shape stays the same: scripts that parse `errors` are not affected).
    {
        std::map<std::string, size_t> counts;
        size_t best = 0;
        std::string best_msg;
        for (const auto &e : r["errors"]) {
            size_t c = ++counts[e.value("message", "")];
            if (c > best) {
                best = c;
                best_msg = e.value("message", "");
            }
        }
        if (best >= 20 && best * 2 >= r["errors"].size()) {
            json w{{"code", "repeated_errors"}, {"count", best}, {"message", best_msg},
                   {"text", std::to_string(best) + " of the " + std::to_string(r["errors"].size()) + " errors listed are the same message: "
                            "group_by: \"message\" returns one entry per message and source with its count and first and last time."}};
            std::string hook = console::hook_of(best_msg);
            if (!hook.empty()) w["hook"] = hook;
            warnings.push_back(w);
        }
    }
    for (auto &e : r["errors"]) {
        std::string m = agent::map_of_load(e.value("map_load", (uint64_t)0));
        if (!m.empty()) e["map"] = m;
        std::string hook = console::hook_of(e.value("message", ""));
        if (!hook.empty()) e["hook"] = hook;
    }
    r["map_load"] = cur_load;
    r["map"] = agent::map_of_load(cur_load);
    if (!warnings.empty()) r["warnings"] = warnings;
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
                          json{{"current", before["result"].value("value", "")},
                               // Lets the MCP layer ask the user directly (elicitation) when the client supports it.
                               {"confirm", {{"arg", "persist"},
                                            {"message", "The agent wants to set the archived convar '" + args["name"].get<std::string>() + "' to '" +
                                                            value + "'. It will be saved to your cfg/config.cfg. Allow it?"}}}});
    json s = agent::rt(r, "convar_set", json{{"name", args["name"]}, {"value", value}});
    if (!s.value("ok", false)) return s;
    wait_frames(3, 3.0);
    json g = agent::rt(r, "convar_get", json{{"name", args["name"]}});
    if (!g.value("ok", false)) return g;
    auto same_value = [&](const std::string &now) {
        if (now == value) return true;
        try {
            return std::stod(now) == std::stod(value);  // numeric normalization (e.g. "1" vs "1.0")
        } catch (...) {
            return false;
        }
    };
    std::string now = g["result"].value("value", "");
    std::string via = "lua";
    bool plain = value.find_first_of("\";\r\n") == std::string::npos;  // nothing that could end the command line
    if (!same_value(now) && plain && agent::config().mode == agent::Mode::Client) {
        // Some convars are silently ignored when Lua sets them (e.g. volume_sfx): retry through the engine console.
        json e = agent::rt(realms::MENU, "engine_cmd", json{{"cmd", args["name"].get<std::string>() + " \"" + value + "\""}});
        if (e.value("ok", false)) {
            wait_frames(3, 3.0);
            json g2 = agent::rt(r, "convar_get", json{{"name", args["name"]}});
            if (g2.value("ok", false)) {
                g = g2;
                now = g["result"].value("value", "");
                via = "engine";
            }
        }
    }
    if (!same_value(now))
        return agent::err("set_refused", "the convar did not take the requested value, from Lua nor from the engine console "
                                         "(cheat without sv_cheats, server convar, or blocked command)",
                          json{{"requested", value}, {"current", now}, {"flags", g["result"]["flag_names"]}});
    json out = g["result"];
    out["previous"] = s["result"]["previous"];
    out["via"] = via;
    if (via == "engine") out["note"] = "ignored when set from Lua: set through the engine console";
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
                        "Runs a console command line from a realm (menu: RunConsoleCommand; server: game.ConsoleCommand; client: "
                        "RunConsoleCommand, with automatic fallback to the engine if the command is blocked from Lua). Like the "
                        "engine console, ';' and line breaks outside quotes separate commands: 'a 1; b 2' runs a then b "
                        "(`statements` lists them). Waits a few frames then returns the console lines printed meanwhile "
                        "(gmod-mcp's own log lines left out; the console is shared, so filter with `regex`). An unknown "
                        "command or convar is an error (unknown_command), not a silent no-op. For every convar it set or "
                        "named, `convars` gives the value read back (value, number, and a warning for FCVAR_ARCHIVE "
                        "convars, which land in the user's config and are shared by every instance). `killserver` on a game "
                        "that hosts a map first leaves it like `disconnect` (left_game_first: true): run in game it can crash "
                        "the engine. Example: {\"cmd\": \"sv_gravity 600\"}. convar_set refuses archived convars unless persist: true.",
                        json{{"properties", {{"cmd", {{"type", "string"}, {"minLength", 1}, {"description", "Command line, e.g. 'echo hello' or 'sv_gravity 600'."}}},
                                             {"realm", realm_p},
                                             {"wait_frames", {{"type", "integer"}, {"minimum", 0}, {"maximum", 600}, {"description", "Frames to wait before reading the output (default 5)."}}},
                                             {"regex", {{"type", "string"}, {"description", "Keep only the output lines matching this (ECMAScript, case-insensitive)."}}},
                                             {"connect_timeout_s", {{"type", "number"}, {"minimum", 1}, {"maximum", 300}, {"description", "For `connect`: how long to wait for the outcome, reported in connect_result (default 20)."}}}}},
                             {"required", {"cmd"}}},
                        json{{"destructiveHint", true}}, console_exec_tool});
    mcp::register_tool({"console_wait", "Wait for console lines",
                        "Blocks until `count` console lines matching `regex` are printed (from `cursor`, default: now), e.g. "
                        "the end line of a test batch: {regex: \"^BATCH DONE\", count: 4, timeout_s: 900}. Returns the "
                        "matching lines and next_cursor (just after the last one used: pass it to the next wait so nothing "
                        "printed in between is missed). Reports progress; error timeout (with the lines found so far). "
                        "Lines come from every realm and the engine (print, Msg, engine messages). Can run as an MCP task.",
                        json{{"properties", {{"regex", {{"type", "string"}, {"minLength", 1}, {"description", "ECMAScript, case-insensitive."}}},
                                             {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10000}, {"description", "Lines to wait for (default 1)."}}},
                                             {"cursor", {{"type", "integer"}, {"minimum", 0}, {"description", "Start from this console cursor (from console_read, console_exec or an earlier wait). Default: now."}}},
                                             {"timeout_s", {{"type", "number"}, {"minimum", 0}, {"maximum", 3600}, {"description", "Default 30."}}}}},
                             {"required", {"regex"}}},
                        RO, console_wait_tool});
    mcp::set_task_support("console_wait", "optional");
    mcp::register_tool({"console_read", "Read the console",
                        "Reads this game's console output: everything printed by the engine and by Lua in every realm (print, Msg, "
                        "MsgN, MsgC, errors), one entry per line, kept in a ring buffer (20000 lines by default, see "
                        "console_buffer) that survives map changes. Without `cursor`: the last `limit` lines (matching `regex`). "
                        "With cursor: the lines from that point on; pass next_cursor to the next call to read only new lines. "
                        "Result: {lines: [{seq, t (s since module start), level: message|warning|assert|error|log, text}], "
                        "next_cursor, lost (lines dropped before you read them), oldest, capture (spew, logging, condebug or none: "
                        "none = nothing is captured)}. To block until a line appears, use console_wait. Example: "
                        "{\"regex\": \"^\\\\[ERROR\\\\]\", \"limit\": 50}.",
                        json{{"properties", {{"cursor", {{"type", "integer"}, {"minimum", 0}, {"description", "Cursor (previous next_cursor)."}}},
                                             {"regex", {{"type", "string"}, {"description", "Optional filter."}}},
                                             {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10000}, {"description", "Default 200."}}}}}},
                        RO, console_read_tool});
    mcp::register_tool({"console_buffer", "Console buffer capacity",
                        "Reads or sets how many console lines this game keeps for console_read, console_wait and console_exec (default "
                        "20000; lowering it drops the oldest lines at once). Without `capacity`: just reads it. Result: {capacity, "
                        "cursor (the current console position)}.",
                        json{{"properties", {{"capacity", {{"type", "integer"}, {"minimum", 16}, {"maximum", 1000000}, {"description", "Number of console lines to keep (16 to 1000000; default 20000). A smaller value drops the oldest lines at once. Omit to just read it."}}}}}},
                        json{{"idempotentHint", true}, {"destructiveHint", false}}, [](const json &a) {
                            if (a.contains("capacity")) console::set_capacity(a["capacity"].get<size_t>());
                            return ok(json{{"capacity", console::capacity()}, {"cursor", console::cursor_now()}});
                        }});
    mcp::register_tool({"errors_since", "Read Lua errors",
                        "Lua errors of every realm of this game, as the game reports them (client mode: the menu's OnLuaError hook; "
                        "srcds: parsed from the console). The game reports each distinct error as it happens. Cursor: 0 = from the "
                        "oldest kept error; pass next_cursor to read only new ones. The last 2000 are kept and survive map loads: "
                        "each error carries map_load (a number that grows with every map load) and map; since_map_load: true "
                        "keeps only the current map's errors, including those raised while it loaded. Result: {errors: [{seq, "
                        "realm, message, stack: [{file, line, function}], addon, addon_id, time, map_load, map, hook?}], next_cursor, "
                        "lost, map_load, map, warnings?}. A game that several agents share, or a stuck loop, fills it with errors that "
                        "are not yours or all the same: group_by: \"message\" returns one entry per message and source, "
                        "{groups: [{message, source, count, first_seq, first_time, last_seq, last_time, realms, map, stack, hook?, addon}], "
                        "groups_total, next_cursor, lost, map_load, map} instead, most frequent first; source / exclude_source keep or drop "
                        "the errors raised under a file prefix (e.g. \"lua/my_addon/client/\"), pattern / exclude_pattern a regex on the message. "
                        "warnings (only when there is something to say): errors_dropped (lost > 0: the oldest errors were dropped before "
                        "you read them) and repeated_errors (most of the list is one message: group it). hook: the hook that raised it, "
                        "when lua_exec / lua_job's hook guard removed or watched it. "
                        "Example: {\"cursor\": 0, \"since_map_load\": true, \"group_by\": \"message\", \"source\": \"lua/my_addon/\"}.",
                        json{{"properties", {{"cursor", {{"type", "integer"}, {"minimum", 0}, {"description", "Start here (default 0 = the oldest kept error)."}}},
                                             {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 2000}, {"description", "Max errors returned (default 100)."}}},
                                             {"realm", realm_prop("Optional realm filter.")},
                                             {"since_map_load", {{"type", "boolean"}, {"description", "Only errors of the current map load (default false)."}}},
                                             {"source", {{"type", "string"}, {"maxLength", 260},
                                                         {"description", "Keep only errors raised under this prefix: the file:line the message starts with (else the first stack frame), "
                                                                         "e.g. \"lua/my_addon/client/\" or \"lua/my_addon/hud.lua:59\". Case-insensitive; backslashes read as slashes."}}},
                                             {"exclude_source", {{"type", "string"}, {"maxLength", 260}, {"description", "Drop the errors raised under this prefix (same form as source)."}}},
                                             {"pattern", {{"type", "string"}, {"maxLength", 500}, {"description", "Keep only errors whose message matches this regular expression (case-insensitive, ECMAScript)."}}},
                                             {"exclude_pattern", {{"type", "string"}, {"maxLength", 500}, {"description", "Drop the errors whose message matches this regular expression."}}},
                                             {"group_by", {{"type", "string"}, {"enum", json::array({"message"})},
                                                           {"description", "\"message\": one entry per distinct message and source instead of the list of errors: {groups: [{message, source, count, first_seq, "
                                                                           "first_time, last_seq, last_time, realms, map, stack (the first one's), hook?, addon}]} most frequent first (limit caps the groups, "
                                                                           "groups_total counts them), next_cursor covers every error read. Default: the plain list."}}}}}},
                        RO, errors_since_tool});
    mcp::register_tool({"errors_clear", "Clear errors", "Forgets the errors kept so far (errors_since then starts empty); cursors keep growing, so an old cursor stays "
                        "valid. Result: {cleared, next_cursor}.", json::object(),
                        json{{"destructiveHint", true}, {"idempotentHint", true}},
                        [](const json &) {
                            console::errors_clear();
                            return ok(json{{"cleared", true}, {"next_cursor", console::errors_cursor()}});
                        }});
    mcp::register_tool({"convar_get", "Read a convar",
                        "Value (string as the engine stores it), number (the value as a number when it is one: compare this, since the "
                        "same value can read \"0.0\" or \"0.000000\"), default, help text, flags (decoded), min/max of a convar. "
                        "FCVAR_PROTECTED convars have their value masked.",
                        json{{"properties", {{"name", {{"type", "string"}, {"minLength", 1}, {"description", "Exact convar name, e.g. 'sv_gravity'. Error not_found if it does not exist."}}}, {"realm", realm_p}}}, {"required", {"name"}}},
                        RO, [](const json &a) { return rt_realm(a, "realm", default_realm(), "convar_get", json{{"name", a["name"]}}); }});
    mcp::register_tool({"convar_set", "Set a convar",
                        "Sets a convar from a realm (RunConsoleCommand; server realm: game.ConsoleCommand), waits 3 frames and reads "
                        "it back. When the value did not take from Lua (some convars, e.g. volume_sfx, ignore Lua) it retries "
                        "through the engine console (client mode). Convars with FCVAR_ARCHIVE are saved into the user's "
                        "cfg/config.cfg when the game quits and are shared by every game of this folder: they are refused unless "
                        "persist: true (your MCP client may then ask the user). Result: the convar as convar_get reads it, plus "
                        "previous and via (lua or engine). Errors: not_found, protected (FCVAR_PROTECTED), archived, set_refused "
                        "(cheat without sv_cheats, wrong realm, blocked command). Example: {\"name\": \"sv_gravity\", \"value\": "
                        "300, \"realm\": \"server\"}.",
                        json{{"properties", {{"name", {{"type", "string"}, {"minLength", 1}, {"description", "Exact convar name, e.g. 'sv_gravity'. Error not_found if it does not exist."}}},
                                             {"value", {{"type", {"string", "number", "boolean"}}, {"description", "New value."}}},
                                             {"realm", realm_p},
                                             {"persist", {{"type", "boolean"}, {"description", "Allow changing an FCVAR_ARCHIVE convar (persisted in config.cfg). Default false."}}}}},
                             {"required", {"name", "value"}}},
                        json{{"destructiveHint", false}, {"idempotentHint", true}}, convar_set_tool});
    mcp::register_tool({"concommand_list", "List commands",
                        "Lists console commands. Always: the Lua commands of `realm` (concommand.Add, name matches `filter` as a "
                        "case-insensitive substring). With a filter and engine: true (default): also the engine's commands and "
                        "convars whose name or help text matches, parsed from the console command `find <filter>`. Result: "
                        "{lua_commands, realm, engine_commands?, engine_convars?}. Example: {\"filter\": \"sv_\"}.",
                        json{{"properties", {{"filter", {{"type", "string"}, {"description", "Case-insensitive substring of the name."}}},
                                             {"realm", realm_p},
                                             {"engine", {{"type", "boolean"}, {"description", "Include engine commands (default true, requires a filter)."}}}}}},
                        RO, concommand_list_tool});
}

}  // namespace tools

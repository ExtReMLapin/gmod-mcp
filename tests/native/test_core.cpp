// Unit tests of the module's tool helpers that need no game: Lua path lookup and the `fields` projection.
// Built as gmod-mcp-coretests (links the module's sources like gmod-mcp-snapshot), run by the CI.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "agent.h"
#include "argcheck.h"
#include "cmdline.h"
#include "console.h"
#include "coordinator.h"
#include "mcp.h"
#include "lua_mount.h"
#include "realms.h"
#include "resources.h"
#include "tools.h"
#include "util.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (cond) {                                                     \
            ++g_pass;                                                   \
        } else {                                                        \
            ++g_fail;                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

static void touch(const fs::path &p) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << "return 1\n";
}

static bool same(const std::string &a, const fs::path &b) {
    std::error_code ec;
    return fs::equivalent(fs::u8path(a), b, ec);
}

// console_exec: statements split like the engine does (T4: "a 1;b 1" went to `a` as the value "1;b"), unknown commands
// detected (PO1: a typo answered ok), gmod-mcp's own log lines kept out of a command's output (SB3).
static void test_cmdline() {
    using V = std::vector<std::string>;
    CHECK(cmdline::split_statements("pba_native_render 1;pba_native_all 1") == (V{"pba_native_render 1", "pba_native_all 1"}));
    CHECK(cmdline::split_statements("  a 1 ;  ; b 2 ;") == (V{"a 1", "b 2"}));
    CHECK(cmdline::split_statements("echo \"x;y\"; c") == (V{"echo \"x;y\"", "c"}));
    CHECK(cmdline::split_statements("a\nb\r\nc") == (V{"a", "b", "c"}));
    CHECK(cmdline::split_statements("sv_cheats 1") == (V{"sv_cheats 1"}));
    CHECK(cmdline::split_statements("").empty());
    CHECK(cmdline::split_statements(" ; ;\n").empty());
    CHECK(cmdline::split_statements("echo \"unclosed;quote") == (V{"echo \"unclosed;quote"}));
    CHECK(cmdline::command_name("  Sv_Cheats 1") == "sv_cheats");
    CHECK(cmdline::command_name("\"quoted name\" 1") == "quoted name");
    CHECK(cmdline::command_name("   ").empty());
    CHECK(cmdline::unknown_commands({"Unknown command \"pba_typo\"", "hello", "Unknown command \"Other\""}) == (V{"pba_typo", "other"}));
    // What GMod really prints (seen live 2026-11-14 in the menu, client and server realms, French client too).
    CHECK(cmdline::unknown_commands({"Unknown command: pba_typo_xyz", "Unknown command: Other_One"}) == (V{"pba_typo_xyz", "other_one"}));
    CHECK(cmdline::unknown_commands({"Unknown command:", "Unknown command: ", "an Unknown command \"y\" inside", "Unknown command: x y", "fine"}) == (V{"x"}));
    // console_exec: connect must be the only statement ("echo x; connect ..." used to skip the check and the disconnect).
    CHECK(!cmdline::connect_not_alone(cmdline::split_statements("connect 127.0.0.1")));
    CHECK(!cmdline::connect_not_alone(cmdline::split_statements("  connect 127.0.0.1:27016 ;")));
    CHECK(cmdline::connect_not_alone(cmdline::split_statements("echo x; connect 127.0.0.1:27016")));
    CHECK(cmdline::connect_not_alone(cmdline::split_statements("connect a; echo b")));
    CHECK(cmdline::connect_not_alone(cmdline::split_statements("Connect a\nECHO b")));
    CHECK(!cmdline::connect_not_alone(cmdline::split_statements("CONNECT 1.2.3.4")));
    CHECK(!cmdline::connect_not_alone(cmdline::split_statements("echo \"x; connect y\"; echo z")));  // quoted ';' is no statement
    CHECK(!cmdline::connect_not_alone(cmdline::split_statements("echo a; echo b")));
    CHECK(!cmdline::connect_not_alone({}));
    CHECK(cmdline::own_log_line("[mcpagent] initialize (protocol 2026-07-28)"));
    CHECK(!cmdline::own_log_line("x [mcpagent] y"));
    CHECK(!cmdline::own_log_line("pba_enable = 1"));
}

// console_wait (RF6, RF7, sweep: agents wrote broken polling loops to know when a long batch was done).
static void test_console_wait() {
    tools::register_console();
    // Lines printed while it waits, mixed with noise: returns once 3 match, with next_cursor just after the third.
    std::thread printer([] {
        for (int i = 0; i < 5; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            console::push_line("message", "noise " + std::to_string(i));
            console::push_line("message", "BATCH DONE " + std::to_string(i));
        }
    });
    json r = mcp::call_tool("console_wait", json{{"regex", "^batch done"}, {"count", 3}, {"timeout_s", 5}});
    printer.join();
    json sc = r.value("structuredContent", json::object());
    CHECK(!r.value("isError", true));
    CHECK(sc.value("matched", 0) == 3);
    CHECK(sc["lines"].size() == 3 && sc["lines"][2]["text"] == "BATCH DONE 2");
    // A second wait from next_cursor sees the 2 lines printed after the third match, not the first ones again.
    json r2 = mcp::call_tool("console_wait", json{{"regex", "^batch done"}, {"count", 2}, {"timeout_s", 1}, {"cursor", sc["next_cursor"]}});
    json sc2 = r2.value("structuredContent", json::object());
    CHECK(!r2.value("isError", true) && sc2["lines"].size() == 2 && sc2["lines"][0]["text"] == "BATCH DONE 3");
    // Without cursor it starts now: lines printed before do not count, and a timeout says how many were found.
    auto t0 = std::chrono::steady_clock::now();
    json r3 = mcp::call_tool("console_wait", json{{"regex", "^batch done"}, {"count", 1}, {"timeout_s", 0.3}});
    double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(r3.value("isError", false));
    CHECK(r3.dump().find("timeout") != std::string::npos);
    CHECK(el >= 0.3 && el < 2.0);
    // Bad regex: invalid_params, at once.
    json r4 = mcp::call_tool("console_wait", json{{"regex", "(unclosed"}, {"timeout_s", 5}});
    CHECK(r4.value("isError", false) && r4.dump().find("invalid_params") != std::string::npos);
}

// errors_since across map loads (RF6: after a reload the agent saw no error and took a false lead). Each error carries
// the map load it happened in; since_map_load keeps only the current one; older errors stay readable.
static void test_errors_map_tags() {
    console::errors_clear();
    agent::note_map_load_start("gm_old");
    console::push_error(json{{"realm", "server"}, {"message", "old map error"}});
    agent::note_map_load_start("gm_new");
    console::push_error(json{{"realm", "client"}, {"message", "new map error"}});
    json all = mcp::call_tool("errors_since", json{{"cursor", 0}}).value("structuredContent", json::object());
    CHECK(all["errors"].size() == 2);
    CHECK(all["errors"].size() == 2 && all["errors"][0].value("map", "") == "gm_old" && all["errors"][1].value("map", "") == "gm_new");
    CHECK(all["errors"].size() == 2 && all["errors"][1].value("map_load", 0) == all["errors"][0].value("map_load", 0) + 1);
    json cur = mcp::call_tool("errors_since", json{{"cursor", 0}, {"since_map_load", true}}).value("structuredContent", json::object());
    CHECK(cur["errors"].size() == 1 && cur["errors"][0].value("message", "") == "new map error");
    CHECK(cur.value("map_load", 0) == all["errors"][1].value("map_load", -1));
}

// errors_since: the since_map_load filter applies before the limit (a limit smaller than the number of older errors
// kept used to hide the current map's errors).
static void test_errors_filter_before_limit() {
    console::errors_clear();
    agent::note_map_load_start("gm_old");
    for (int i = 0; i < 150; ++i) console::push_error(json{{"realm", "server"}, {"message", "old " + std::to_string(i)}});
    agent::note_map_load_start("gm_cur");
    console::push_error(json{{"realm", "server"}, {"message", "current 1"}});
    console::push_error(json{{"realm", "client"}, {"message", "current 2"}});
    json r = mcp::call_tool("errors_since", json{{"cursor", 0}, {"limit", 100}, {"since_map_load", true}}).value("structuredContent", json::object());
    CHECK(r["errors"].size() == 2 && r["errors"][0].value("message", "") == "current 1");
    // The limit counts kept errors only, and next_cursor lets the caller continue exactly after the last one returned.
    json r1 = mcp::call_tool("errors_since", json{{"cursor", 0}, {"limit", 1}, {"since_map_load", true}}).value("structuredContent", json::object());
    CHECK(r1["errors"].size() == 1 && r1["errors"][0].value("message", "") == "current 1");
    json r2 = mcp::call_tool("errors_since", json{{"cursor", r1.value("next_cursor", 0)}, {"limit", 1}, {"since_map_load", true}}).value("structuredContent", json::object());
    CHECK(r2["errors"].size() == 1 && r2["errors"][0].value("message", "") == "current 2");
    // The realm filter too.
    json rc = mcp::call_tool("errors_since", json{{"cursor", 0}, {"limit", 1}, {"realm", "client"}}).value("structuredContent", json::object());
    CHECK(rc["errors"].size() == 1 && rc["errors"][0].value("message", "") == "current 2");
}

static json errs_current(const char *realm = nullptr) {
    json a{{"cursor", 0}, {"since_map_load", true}};
    if (realm) a["realm"] = realm;
    return mcp::call_tool("errors_since", a).value("structuredContent", json::object());
}
static bool has_msg(const json &r, const std::string &m) {
    for (const auto &e : r["errors"])
        if (e.value("message", "") == m) return true;
    return false;
}

// A map load starts when the NEW server Lua state appears, not when the pump later notices it: the server autorun runs
// inside the load, before the next frame. These are the calls the module makes (module.cpp gmod13_open/close in the
// server state, agent::pump), in the order the game produces them.
static void test_errors_load_stamp_order() {
    // Client mode. Map 1 loads: the autorun loader requires the module in the new server state, an autorun errors after it.
    console::errors_clear();
    agent::on_server_state_open();
    uint64_t l1 = agent::map_load_seq();
    console::push_error(json{{"realm", "server"}, {"message", "m1 autorun"}});
    agent::server_realm_appeared();  // the pump sees the server realm: same load, no new number
    CHECK(agent::map_load_seq() == l1);
    console::push_error(json{{"realm", "server"}, {"message", "m1 later"}});
    // Map change. The old server state closes, then the new one is created: a shared autorun file (lua/autorun/*.lua) and
    // server autorun files sorted before the loader run BEFORE the module is required in the new state.
    agent::on_server_state_close();
    console::push_error(json{{"realm", "server"}, {"message", "m2 early autorun"}});
    console::push_error(json{{"realm", "menu"}, {"message", "menu error"}});
    agent::on_server_state_open();
    CHECK(agent::map_load_seq() == l1 + 1);
    console::push_error(json{{"realm", "server"}, {"message", "m2 autorun"}});
    console::push_error(json{{"realm", "client"}, {"message", "m2 client"}});
    agent::server_realm_appeared();  // the pump finds the realm back later: still the same load
    CHECK(agent::map_load_seq() == l1 + 1);
    json cur = errs_current();
    CHECK(cur["errors"].size() == 3);
    CHECK(has_msg(cur, "m2 early autorun") && has_msg(cur, "m2 autorun") && has_msg(cur, "m2 client"));
    CHECK(!has_msg(cur, "m1 autorun") && !has_msg(cur, "m1 later") && !has_msg(cur, "menu error"));
    CHECK(errs_current("server")["errors"].size() == 2);
    // Pump first, loader after (the pump ran between the state's creation and the autorun): still one number per map.
    agent::on_server_state_close();
    agent::server_realm_appeared();
    uint64_t l3 = agent::map_load_seq();
    CHECK(l3 == l1 + 2);
    console::push_error(json{{"realm", "server"}, {"message", "m3 first"}});
    agent::on_server_state_open();
    CHECK(agent::map_load_seq() == l3);
    console::push_error(json{{"realm", "server"}, {"message", "m3 second"}});
    json c3 = errs_current();
    CHECK(c3["errors"].size() == 2 && has_msg(c3, "m3 first") && has_msg(c3, "m3 second"));

    // Dedicated server: the pump runs from the server state, so it never sees the realm absent; only the module's
    // open/close in each server state tell. The number must still grow with every map.
    console::errors_clear();
    agent::on_server_state_open();
    uint64_t d1 = agent::map_load_seq();
    agent::server_realm_appeared();  // first pump of the first state
    console::push_error(json{{"realm", "server"}, {"message", "d1"}});
    for (int i = 2; i <= 3; ++i) {
        agent::on_server_state_close();
        console::push_error(json{{"realm", "server"}, {"message", "d" + std::to_string(i) + " early"}});
        agent::on_server_state_open();  // the pump is re-registered in the new state, it does not see an absence
        CHECK(agent::map_load_seq() == d1 + (uint64_t)(i - 1));
        console::push_error(json{{"realm", "server"}, {"message", "d" + std::to_string(i)}});
    }
    json d = errs_current();
    CHECK(d["errors"].size() == 2 && has_msg(d, "d3 early") && has_msg(d, "d3"));
    json all = mcp::call_tool("errors_since", json{{"cursor", 0}}).value("structuredContent", json::object());
    CHECK(all["errors"].size() == 5);
}

// B6: small things agents tripped on.
static void test_small_b6() {
    // sweep: read-only tools that answer the same on every instance work without `instance`.
    for (const char *t : {"status", "wiki_search", "wiki_page", "map_list"}) CHECK(mcp::instance_independent(t));
    for (const char *t : {"lua_exec", "console_exec", "ents_find", "api_search", "lua_check", "convar_get"}) CHECK(!mcp::instance_independent(t));
    // P2: status {fields} keeps only the named top-level keys.
    json st = tools::ok(json{{"map", "gm_construct"}, {"fps_max", 20}, {"realms", {{"server", true}}}, {"perf", {{"fps", 19.8}}}});
    json pr = tools::project_keys(st, json{{"fields", {"map", "fps_max", "missing"}}});
    CHECK(pr["result"].size() == 3 && pr["result"]["map"] == "gm_construct" && pr["result"]["fps_max"] == 20);
    CHECK(pr["result"].contains("projected"));
    CHECK(tools::project_keys(st, json::object()) == st);
    json err = agent::err("x", "y");
    CHECK(tools::project_keys(err, json{{"fields", {"map"}}}) == err);
    // T4: an instance that died says so, with its pid, instead of a bare network error.
    std::string dead = coordinator::gone_message("client-123", 4242, 12.5, false);
    CHECK(dead.find("client-123") != std::string::npos && dead.find("4242") != std::string::npos);
    CHECK(dead.find("exited") != std::string::npos && dead.find("instances_launch") != std::string::npos);
    std::string hung = coordinator::gone_message("client-123", 4242, 12.5, true);
    CHECK(hung.find("still running") != std::string::npos && hung.find("game_terminate") != std::string::npos);
    // The pid of a removed member may be reused: only the same start stamp means the same process.
    CHECK(!coordinator::same_process_running(0, 12345));       // no such process
    CHECK(!coordinator::same_process_running(0, 0));
    CHECK(coordinator::same_process_running(12345, 12345));    // still the process we registered
    CHECK(!coordinator::same_process_running(99999, 12345));   // pid reused by another process
    CHECK(coordinator::same_process_running(99999, 0));        // never recorded: cannot tell, assume alive
    CHECK(coordinator::same_process_running(1, 12345));        // unreadable (access denied): assume alive
    CHECK(coordinator::same_process_running(12345, 1));
}

// B1 lua_mount: which folders are accepted as a mount root (no game needed for this part).
static void test_lua_mount_paths() {
    fs::path t = fs::temp_directory_path() / "gmod-mcp-coretests-mounted folder";
    std::error_code ec;
    fs::remove_all(t, ec);
    touch(t / "addon" / "lua" / "autorun" / "x.lua");
    touch(t / "my addon wt" / "lua" / "x.lua");
    touch(t / "nolua" / "materials" / "m.vmt");
    touch(t / "file.txt");
    std::string root, err;
    auto ok = [&](const fs::path &in, const fs::path &want) {
        root.clear();
        err.clear();
        bool r = lua_mount::normalize_root(in.u8string(), root, err);
        return r && same(root, want) && root.back() != '/' && root.back() != '\\';
    };
    auto bad = [&](const std::string &in, const char *word) {
        root.clear();
        err.clear();
        bool r = lua_mount::normalize_root(in, root, err);
        return !r && root.empty() && err.find(word) != std::string::npos;
    };
    CHECK(ok(t / "addon", t / "addon"));                                   // the addon root
    CHECK(ok(t / "addon" / "lua", t / "addon"));                           // its lua/ folder
    CHECK(ok(fs::u8path((t / "addon" / "lua").u8string() + "/"), t / "addon"));  // trailing separator
#ifdef _WIN32
    CHECK(ok(fs::u8path((t / "addon").u8string() + "\\"), t / "addon"));  // a backslash is a separator on Windows only
#endif
    CHECK(ok(t / "my addon wt", t / "my addon wt"));                       // spaces are fine
    CHECK(bad((t / "nolua").u8string(), "lua"));                           // no lua/ inside
    CHECK(bad((t / "missing").u8string(), "not found"));
    CHECK(bad((t / "file.txt").u8string(), "not a folder"));
    CHECK(bad("addons/pba", "absolute"));
    CHECK(bad("", "empty"));
    CHECK(bad((t / "addon").u8string() + "\"", "character"));
    CHECK(bad((t / "addon").u8string() + ";C:/x", "character"));
    CHECK(bad((t / "addon").u8string() + "\nx", "character"));
    // The module reads back what the bridge puts on the client's command line (quoted, spaces, backslashes).
    auto args = util::split_args(R"("C:\Games\gmod_win64.exe" -multirun -mcp -mcp_lua_mount "D:\dev\my addon wt" +fps_max 20)");
    CHECK(util::flag_value(args, "-mcp_lua_mount") == R"(D:\dev\my addon wt)");
    CHECK(util::flag_value(util::split_args("gmod -mcp"), "-mcp_lua_mount").empty());
    // The tool refuses a bad path at once (before touching the game's main thread).
    tools::register_lua_mount();
    json r = mcp::call_tool("lua_mount", json{{"path", "addons/pba"}});
    CHECK(r.value("isError", false) && r.dump().find("invalid_params") != std::string::npos);
    r = mcp::call_tool("lua_mount", json{{"path", (t / "nolua").u8string()}});
    CHECK(r.value("isError", false) && r.dump().find("lua") != std::string::npos);
}

// srcds: Lua errors come from the console. An error in code without a .lua file name (lua_exec, CompileString chunks,
// "=name" chunks) was missed: seen live on a Windows srcds 2026-11-14.
static void test_srcds_error_parsing() {
    console::errors_clear();
    uint64_t c0 = console::errors_cursor();
    console::set_parse_errors_from_console(true);
    console::push_line("message", "[ERROR] mcpagent:lua_exec:1: boom from a chunk");
    console::push_line("message", "  1. unknown - mcpagent:lua_exec:1");
    console::push_line("message", "[ERROR] addons/x/lua/autorun/x.lua:3: boom from a file");
    console::push_line("message", "  1. fn - addons/x/lua/autorun/x.lua:3");
    console::push_line("message", "  2. unknown - [C]:-1");
    console::push_line("message", "[my_addon] lua/y.lua:7: boom with an addon tag");
    console::push_line("message", "a normal line");
    console::push_line("message", "[ERROR] not an error line without a line number");
    console::push_line("message", "flush");
    console::set_parse_errors_from_console(false);
    json r = console::errors_since(c0, 100)["errors"];
    CHECK(r.size() == 3);
    CHECK(r.size() == 3 && r[0]["message"] == "mcpagent:lua_exec:1: boom from a chunk" && r[0]["stack"].size() == 1);
    CHECK(r.size() == 3 && r[1]["message"] == "addons/x/lua/autorun/x.lua:3: boom from a file" && r[1]["stack"].size() == 2);
    CHECK(r.size() == 3 && r[2]["addon"] == "my_addon");
}

// Engine print callbacks must never throw (a C++ exception through the engine kills the game), and error parsing
// must not run a regex on a huge line.
static void test_console_never_throws() {
    console::errors_clear();
    uint64_t c0 = console::errors_cursor();
    console::set_parse_errors_from_console(true);
    bool threw = false;
    try {
        console::push_line("message", "[ERROR] mcpagent:lua_exec:1: head one");
        console::push_line("message", "  1. fn - lua/a.lua:3");
        console::push_line("message", "  99999999999999999999999999999999999999999. fn - lua/a.lua:4");  // 40-digit frame number
        console::push_line("message", "  2. fn - lua/a.lua:99999999999999999999999999999999999999999");  // 40-digit line number
        console::push_line("message", "  3. fn - lua/a.lua:5");
        console::push_line("message", "[ERROR] " + std::string(65536, 'x') + ":1: " + std::string(65536, 'y'));  // 64 KB line
        console::push_line("message", std::string(65536, ' ') + "1. fn - " + std::string(65536, 'f') + ":1");
        console::push_line("message", "[ERROR] mcpagent:lua_exec:2: head two");
        console::push_line("message", "flush");
    } catch (...) {
        threw = true;
    }
    console::set_parse_errors_from_console(false);
    CHECK(!threw);
    json r = console::errors_since(c0, 100)["errors"];
    // The two real errors are recorded; the 64 KB line is no error; the garbage frames are not recorded as frames.
    CHECK(r.size() == 2);
    CHECK(r.size() == 2 && r[0]["message"] == "mcpagent:lua_exec:1: head one");
    CHECK(r.size() == 2 && r[0]["stack"].size() == 2 && r[0]["stack"][0]["line"] == 3 && r[0]["stack"][1]["line"] == 5);
    CHECK(r.size() == 2 && r[1]["message"] == "mcpagent:lua_exec:2: head two");
    console::errors_clear();
}

// console::read used to run the caller's regex while holding the lock every engine print takes.
static void test_console_read_regex_outside_lock() {
    size_t cap = console::capacity();
    console::set_capacity(200000);
    uint64_t c0 = console::cursor_now();
    // Lines that make std::regex take a long time (quadratic backtracking) with the pattern below, without hitting its
    // own complexity limit.
    for (int i = 0; i < 400; ++i) console::push_line("message", "RXL " + std::string(300, 'x') + " " + std::to_string(i));
    std::atomic<bool> started{false}, done{false};
    console::ReadResult rr{{}, 0, 0, 0};
    std::string err;
    std::thread reader([&] {
        started = true;
        rr = console::read(c0, 1000, "x.*y.*z", err);
        done = true;
    });
    while (!started) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    bool still_scanning = !done;
    double worst = 0;
    for (int i = 0; i < 5; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        console::push_line("message", "PRINT DURING SCAN " + std::to_string(i));
        worst = std::max(worst, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(still_scanning);  // otherwise the test proves nothing: make the lines costlier
    CHECK(worst < 50.0);
    if (worst >= 50.0) std::printf("  push_line blocked %.0f ms during a regex scan\n", worst);
    reader.join();
    CHECK(err.empty() && rr.lines.empty());
    // Semantics unchanged: limit, next_cursor, lost, oldest.
    std::string e2;
    console::ReadResult a = console::read(c0, 2, "^RXL", e2);
    CHECK(a.lines.size() == 2 && a.lines[0].seq == c0 && a.lines[1].seq == c0 + 1 && a.next_cursor == c0 + 2 && a.lost == 0);
    console::ReadResult b = console::read(c0, 100000, "^PRINT", e2);
    CHECK(!b.lines.empty() && b.next_cursor == console::cursor_now());
    console::set_capacity(cap);
}

// Edge cases (2026-11-14).
static void test_edge_cases() {
    using V = std::vector<std::string>;
    // Statements: quotes, line breaks, many statements, blanks, long lines, unicode.
    CHECK(cmdline::split_statements("say \"a;b\nc\"") == (V{"say \"a;b", "c\""}));  // a line break ends a quoted string
    std::string many;
    for (int i = 0; i < 100; ++i) many += "echo " + std::to_string(i) + ";";
    CHECK(cmdline::split_statements(many).size() == 100);
    CHECK(cmdline::split_statements(" \t ; \t\r\n ").empty());
    std::string longline = "echo " + std::string(5000, 'x');
    CHECK(cmdline::split_statements(longline) == (V{longline}));
    CHECK(cmdline::split_statements("say \xC3\xA9t\xC3\xA9;say \xE2\x82\xAC") == (V{"say \xC3\xA9t\xC3\xA9", "say \xE2\x82\xAC"}));
    CHECK(cmdline::command_name("\xC3\x89T\xC3\x89 1") == "\xC3\x89t\xC3\x89" || !cmdline::command_name("\xC3\x89T\xC3\x89 1").empty());
    // Network rule: a connect anywhere in the line is caught, whatever hides it.
    for (const char *line : {"echo a; connect 1.2.3.4", "echo \"x;y\"; connect 8.8.8.8", "echo a\nconnect 1.2.3.4",
                             "echo a\r\nconnect 1.2.3.4", "CONNECT 1.2.3.4", "  connect   1.2.3.4  ", "redirect 1.2.3.4",
                             "retry", "connect", "echo \"a\nconnect 1.2.3.4", "echo \"a\rconnect 1.2.3.4",
                             "connect_lobby 1", "\"connect\" 1.2.3.4"}) {
        bool refused = !tools::forbidden_network_cmd(line).empty();
        if (!refused) std::printf("  not refused: [%s]\n", line);
        CHECK(refused);
    }
    for (const char *line : {"echo connect 1.2.3.4", "echo \"connect 1.2.3.4\"", "sv_gravity 600", "connect 127.0.0.1:27015",
                             "connect localhost"})
        CHECK(tools::forbidden_network_cmd(line).empty());
    // console_wait: a cursor in the future starts now; more lines asked than printed times out with what was found;
    // lines lost to a tiny buffer are reported.
    size_t cap = console::capacity();
    json r = mcp::call_tool("console_wait", json{{"regex", "^EDGE"}, {"cursor", console::cursor_now() + 1000000}, {"timeout_s", 0.2}});
    CHECK(r.value("isError", false) && r.dump().find("timeout") != std::string::npos);
    uint64_t c0 = console::cursor_now();
    console::push_line("message", "EDGE 1");
    r = mcp::call_tool("console_wait", json{{"regex", "^EDGE"}, {"cursor", c0}, {"count", 3}, {"timeout_s", 0.2}});
    CHECK(r.value("isError", false) && r.dump().find("EDGE 1") != std::string::npos);
    console::set_capacity(16);
    c0 = console::cursor_now();
    for (int i = 0; i < 100; ++i) console::push_line("message", "FLOOD " + std::to_string(i));
    r = mcp::call_tool("console_wait", json{{"regex", "^FLOOD 99$"}, {"cursor", c0}, {"timeout_s", 1}});
    json sc = r.value("structuredContent", json::object());
    CHECK(!r.value("isError", true) && sc.value("lost", 0) > 0);
    console::set_capacity(cap);
    // Errors: more than the buffer holds -> lost counted; a cursor past the end returns nothing.
    console::errors_clear();
    uint64_t e0 = console::errors_cursor();
    for (int i = 0; i < 2100; ++i) console::push_error(json{{"realm", "server"}, {"message", "e" + std::to_string(i)}});
    json es = console::errors_since(e0, 5000);
    CHECK(es["errors"].size() == 2000 && es.value("lost", 0) == 100);
    json past = console::errors_since(e0 + 100000, 10);
    CHECK(past["errors"].empty());
    console::errors_clear();
    // A non-UTF-8 byte in a console line or a path never breaks a response.
    console::push_line("message", "bad \xFB byte");
    json rd = mcp::call_tool("console_read", json{{"limit", 5}});
    CHECK(!rd.value("isError", true));
    bool dumped = true;
    try {
        (void)rd.dump(-1, ' ', false, json::error_handler_t::replace);
    } catch (...) {
        dumped = false;
    }
    CHECK(dumped);
    std::string saved = agent::config().game_dir;
    agent::config().game_dir = "D:/Jeux vid\xE9o/garrysmod";  // a path in a legacy code page
    bool read_ok = true;
    try {
        json cfgres = resources::read("mcpagent://config");
        read_ok = cfgres.contains("text");
    } catch (...) {
        read_ok = false;
    }
    CHECK(read_ok);
    agent::config().game_dir = saved;
    // Mounted folder paths.
    fs::path t = fs::temp_directory_path() / "gmod-mcp-edge-mount";
    std::error_code ec;
    fs::remove_all(t, ec);
    fs::create_directories(t / "\xC3\xA9t\xC3\xA9 wt" / "lua");
    fs::create_directories(t / "garrysmod" / "lua");
    fs::create_directories(t / "garrysmod" / "addons" / "pba" / "lua");
    std::ofstream(t / "afile.txt") << "x";
    std::string root, err;
    // The engine's file system does not find files under a non-ASCII path (seen live 2026-11-14: a mounted folder with
    // accents was listed in the search paths but none of its files loaded): refused, with the way out.
    CHECK(!lua_mount::normalize_root((t / "\xC3\xA9t\xC3\xA9 wt").u8string(), root, err) && err.find("ASCII") != std::string::npos &&
          err.find("mklink /J") != std::string::npos);
    CHECK(!lua_mount::normalize_root((t / "\xC3\xA9t\xC3\xA9 wt" / "lua").u8string() + "/", root, err));
    CHECK(!lua_mount::normalize_root((t / "afile.txt").u8string(), root, err) && err.find("not a folder") != std::string::npos);
    CHECK(!lua_mount::normalize_root("\\\\nosuchserver\\share\\wt", root, err));
    CHECK(!lua_mount::normalize_root(t.u8string() + "/does-not-exist", root, err) && err.find("not found") != std::string::npos);
    std::string game = (t / "garrysmod").u8string();
    CHECK(!lua_mount::refuse_inside_game((t / "garrysmod").u8string(), game).empty());
    CHECK(!lua_mount::refuse_inside_game((t / "garrysmod" / "addons" / "pba").u8string(), game).empty());
    CHECK(!lua_mount::refuse_inside_game(game + "/", game).empty());
    CHECK(lua_mount::refuse_inside_game((t / "\xC3\xA9t\xC3\xA9 wt").u8string(), game).empty());
    CHECK(lua_mount::refuse_inside_game((t / "garrysmod-wt").u8string(), game).empty());  // a sibling with the same prefix
    fs::remove_all(t, ec);
}

// Bridge tools' argument check (edge cases 2026-11-14: count 0 started a launch with nothing, lua_mount: 5 and
// preset: "turbo" were ignored and real clients were started).
static void test_argcheck() {
    json schema = json::parse(R"({"type":"object","additionalProperties":false,"required":["name"],"properties":{
        "name":{"type":"string","minLength":1,"maxLength":8,"pattern":"^[a-z]+$"},
        "count":{"type":"integer","minimum":1,"maximum":8},
        "ratio":{"type":"number","minimum":0},
        "preset":{"type":"string","enum":["light","normal"]},
        "flag":{"type":"boolean"},
        "target":{"type":["string","null"]},
        "pids":{"type":"array","items":{"type":"integer","minimum":1},"minItems":1,"maxItems":3}}})");
    auto ok = [&](const char *a) { return argcheck::check(schema, json::parse(a)).empty(); };
    auto bad = [&](const char *a, const char *word) {
        std::string e = argcheck::check(schema, json::parse(a));
        return !e.empty() && e.find(word) != std::string::npos;
    };
    CHECK(ok(R"({"name":"abc"})"));
    CHECK(ok(R"({"name":"abc","count":8,"ratio":0.5,"preset":"light","flag":true,"target":null,"pids":[1,2]})"));
    CHECK(ok(R"({"name":"abc","target":"x","count":1.0})"));  // 1.0 is an integer in JSON Schema
    CHECK(bad(R"({})", "name"));
    CHECK(bad(R"({"name":"abc","count":0})", "count"));
    CHECK(bad(R"({"name":"abc","count":9})", "count"));
    CHECK(bad(R"({"name":"abc","count":"two"})", "count"));
    CHECK(bad(R"({"name":"abc","count":1.5})", "count"));
    CHECK(bad(R"({"name":"abc","ratio":-1})", "ratio"));
    CHECK(bad(R"({"name":"abc","preset":"turbo"})", "preset"));
    CHECK(bad(R"({"name":"abc","flag":"yes"})", "flag"));
    CHECK(bad(R"({"name":"abc","target":5})", "target"));
    CHECK(bad(R"({"name":"abc","pids":"abc"})", "pids"));
    CHECK(bad(R"({"name":"abc","pids":[]})", "pids"));
    CHECK(bad(R"({"name":"abc","pids":[1,2,3,4]})", "pids"));
    CHECK(bad(R"({"name":"abc","pids":[0]})", "pids"));
    CHECK(bad(R"({"name":""})", "name"));
    CHECK(bad(R"({"name":"abcdefghi"})", "name"));
    CHECK(bad(R"({"name":"ABC"})", "name"));
    CHECK(bad(R"({"name":"abc","other":1})", "other"));
    CHECK(bad(R"([1,2])", "object"));
    CHECK(argcheck::check(json::object(), json::parse(R"({"anything":1})")).empty());  // no properties declared: anything
}

// lua_job ids carry the realm's generation ("server-g3-1"): job_seq restarts in every new Lua state, so "server-1" of
// one map is another job on the next map. A status/cancel with an id of an earlier generation must say realm_reset
// and never reach the new state's jobs.
static void test_job_ids() {
    tools::JobId a = tools::parse_job_id("server-g3-1");
    CHECK(a.realm == realms::SERVER && a.has_generation && a.generation == 3);
    tools::JobId b = tools::parse_job_id("client-g12-340");
    CHECK(b.realm == realms::CLIENT && b.has_generation && b.generation == 12);
    tools::JobId m = tools::parse_job_id("menu-g1-7");
    CHECK(m.realm == realms::MENU && m.generation == 1);
    // Old form (no generation): the realm still parses, nothing to compare.
    tools::JobId old = tools::parse_job_id("server-3");
    CHECK(old.realm == realms::SERVER && !old.has_generation);
    // Garbage.
    CHECK(tools::parse_job_id("").realm < 0);
    CHECK(tools::parse_job_id("nodash").realm < 0);
    CHECK(tools::parse_job_id("bogus-g1-1").realm < 0);
    CHECK(tools::parse_job_id("server-g-1").realm == realms::SERVER && !tools::parse_job_id("server-g-1").has_generation);
    CHECK(!tools::parse_job_id("server-gx-1").has_generation);
    CHECK(tools::parse_job_id("server-g99999999999999999999999-1").realm < 0);
    // The decision: same generation keeps the job reachable, another generation is realm_reset.
    CHECK(tools::job_reset_message(a, 3).empty());
    CHECK(!tools::job_reset_message(a, 4).empty());
    CHECK(tools::job_reset_message(a, 4).find("server realm was re-created") != std::string::npos);
    CHECK(tools::job_reset_message(a, 4).find("job is gone") != std::string::npos);
    CHECK(tools::job_reset_message(b, 13).find("client realm") != std::string::npos);
    CHECK(tools::job_reset_message(old, 99).empty());  // no generation in the id: the Lua side answers not_found
}

int main() {
    test_cmdline();
    test_console_wait();
    test_errors_map_tags();
    test_errors_filter_before_limit();
    test_errors_load_stamp_order();
    test_small_b6();
    test_srcds_error_parsing();
    test_console_never_throws();
    test_console_read_regex_outside_lock();
    test_edge_cases();
    test_argcheck();
    test_job_ids();
    test_lua_mount_paths();
    fs::path root = fs::temp_directory_path() / "gmod-mcp-coretests" / "garrysmod";
    std::error_code ec;
    fs::remove_all(root.parent_path(), ec);
    touch(root / "lua/mcpagent_tmp/t.lua");
    touch(root / "lua/mcpagent_tmp/shadow/x.lua");
    touch(root / "lua/autorun/a.lua");
    touch(root / "addons/pba/lua/pba/core.lua");
    touch(root / "addons/pba/lua/autorun/pba_init.lua");
    touch(root / "gamemodes/sandbox/gamemode/init.lua");
    auto &cfg = agent::config();
    cfg.game_dir = root.u8string();
    cfg.tmp_dir = (root / "lua/mcpagent_tmp").u8string();

    std::string abs, err;
    // Temporary directory first (unchanged behaviour).
    CHECK(tools::resolve_lua_path("t.lua", false, abs, err) && same(abs, root / "lua/mcpagent_tmp/t.lua"));
    // RETEX 2026-11-13 #6: a path relative to the game folder, file or folder.
    CHECK(tools::resolve_lua_path("addons/pba/lua", true, abs, err) && same(abs, root / "addons/pba/lua"));
    CHECK(tools::resolve_lua_path("addons/pba/lua/", true, abs, err) && same(abs, root / "addons/pba/lua"));
    CHECK(tools::resolve_lua_path("addons\\pba\\lua", true, abs, err) && same(abs, root / "addons/pba/lua"));
    CHECK(tools::resolve_lua_path("addons/pba/lua/pba/core.lua", false, abs, err) && same(abs, root / "addons/pba/lua/pba/core.lua"));
    // A folder is not a file.
    CHECK(!tools::resolve_lua_path("addons/pba/lua", false, abs, err));
    // include() style names: lua/, addons/*/lua/, gamemodes/.
    CHECK(tools::resolve_lua_path("autorun/a.lua", false, abs, err) && same(abs, root / "lua/autorun/a.lua"));
    CHECK(tools::resolve_lua_path("pba/core.lua", false, abs, err) && same(abs, root / "addons/pba/lua/pba/core.lua"));
    CHECK(tools::resolve_lua_path("autorun/pba_init.lua", false, abs, err) && same(abs, root / "addons/pba/lua/autorun/pba_init.lua"));
    CHECK(tools::resolve_lua_path("sandbox/gamemode/init.lua", false, abs, err) && same(abs, root / "gamemodes/sandbox/gamemode/init.lua"));
    CHECK(tools::resolve_lua_path("./t.lua", false, abs, err) && same(abs, root / "lua/mcpagent_tmp/t.lua"));
    // Absolute paths, file and folder.
    CHECK(tools::resolve_lua_path((root / "addons/pba/lua").u8string(), true, abs, err));
    CHECK(!tools::resolve_lua_path((root / "addons/pba/lua").u8string(), false, abs, err));
    // Not found: the error lists every place tried.
    err.clear();
    CHECK(!tools::resolve_lua_path("addons/nope/lua", true, abs, err));
    CHECK(err.find("tried") != std::string::npos && err.find("mcpagent_tmp") != std::string::npos &&
          err.find("pba") != std::string::npos);
    // Escapes refused.
    CHECK(!tools::resolve_lua_path("../cfg/config.cfg", false, abs, err));
    CHECK(!tools::resolve_lua_path("addons/../../x.lua", false, abs, err));
    CHECK(!tools::resolve_lua_path("C:relative.lua", false, abs, err));
    // resolve_lua_file gives the chunk name relative to the game folder.
    std::string chunk;
    CHECK(tools::resolve_lua_file("pba/core.lua", abs, chunk, err) && chunk == "@addons/pba/lua/pba/core.lua");

    // Mounted Lua folder (lua_mount): the tools look where the engine looks, in the same order (the mounted folder first), and
    // name its files like the engine (relative to the mount root: "@lua/x.lua"). Seen live 2026-11-14: lua_reload,
    // lua_check and lua_function_source did not find a file that only exists in the mounted folder.
    {
        fs::path ov = root.parent_path() / "ov wt";
        touch(ov / "lua/ovx/sh.lua");
        touch(ov / "lua/pba/core.lua");           // shadows the installed addon's copy
        touch(ov / "gamemodes/ovgm/gamemode/init.lua");
        lua_mount::set_current_for_tests(ov.u8string());
        CHECK(tools::resolve_lua_file("ovx/sh.lua", abs, chunk, err) && same(abs, ov / "lua/ovx/sh.lua") && chunk == "@lua/ovx/sh.lua");
        CHECK(tools::resolve_lua_file("pba/core.lua", abs, chunk, err) && same(abs, ov / "lua/pba/core.lua") && chunk == "@lua/pba/core.lua");
        CHECK(tools::resolve_lua_file("lua/ovx/sh.lua", abs, chunk, err) && same(abs, ov / "lua/ovx/sh.lua"));  // a source name
        CHECK(tools::resolve_lua_file("ovgm/gamemode/init.lua", abs, chunk, err) && same(abs, ov / "gamemodes/ovgm/gamemode/init.lua") &&
              chunk == "@gamemodes/ovgm/gamemode/init.lua");
        CHECK(tools::resolve_lua_path("ovx", true, abs, err) && same(abs, ov / "lua/ovx"));  // a folder (lua_check)
        CHECK(tools::resolve_lua_file("t.lua", abs, chunk, err) && same(abs, root / "lua/mcpagent_tmp/t.lua"));  // tmp still first
        CHECK(tools::resolve_lua_file("autorun/a.lua", abs, chunk, err) && same(abs, root / "lua/autorun/a.lua"));  // not shadowed
        CHECK(tools::resolve_lua_file((ov / "lua/ovx/sh.lua").u8string(), abs, chunk, err) && chunk == "@lua/ovx/sh.lua");  // absolute
        // lua_reload: the mounted folder's copy first, the addon's listed after it.
        auto all = tools::find_game_lua("pba/core.lua", err);
        CHECK(all.size() == 2 && same(all[0], ov / "lua/pba/core.lua") && same(all[1], root / "addons/pba/lua/pba/core.lua"));
        all = tools::find_game_lua("ovx/sh.lua", err);
        CHECK(all.size() == 1 && same(all[0], ov / "lua/ovx/sh.lua"));
        err.clear();
        CHECK(!tools::resolve_lua_path("nope.lua", false, abs, err) && err.find("ov wt") != std::string::npos);  // listed when missing
        lua_mount::set_current_for_tests("");
        CHECK(tools::resolve_lua_file("pba/core.lua", abs, chunk, err) && same(abs, root / "addons/pba/lua/pba/core.lua"));
        CHECK(!tools::resolve_lua_file("ovx/sh.lua", abs, chunk, err));
    }

    // `fields` projection.
    json r{{"ok", true}, {"result", {{"values", json::array({json{{"pass", 3}, {"fails", json::array()}, {"log", std::string(5000, 'x')}}, 7})}, {"n", 2}}}};
    json p = tools::project_values(r, json{{"fields", {"pass", "fails", "missing"}}});
    CHECK(p["result"]["values"][0] == (json{{"pass", 3}, {"fails", json::array()}}));
    CHECK(p["result"]["values"][1] == 7);
    CHECK(p["result"]["projected"].is_array());
    CHECK(tools::project_values(r, json::object()) == r);  // no fields: untouched
    json e{{"ok", false}, {"error", {{"code", "lua_error"}}}};
    CHECK(tools::project_values(e, json{{"fields", {"pass"}}}) == e);  // errors untouched

    fs::remove_all(root.parent_path(), ec);
    std::printf("core: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

// Unit tests of the module's tool helpers that need no game: Lua path lookup and the `fields` projection.
// Built as gmod-mcp-coretests (links the module's sources like gmod-mcp-snapshot), run by the CI.
#ifdef _WIN32
#include <winsock2.h>
#endif
#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <set>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

#include "agent.h"
#include "argcheck.h"
#include "cmdline.h"
#include "console.h"
#include "coordinator.h"
#include "http_server.h"
#include "installer.h"
#include "crashinfo.h"
#include "mcp.h"
#include "next_call.h"
#include "output_schemas.h"
#include "lua_mount.h"
#include "realms.h"
#include "resources.h"
#include "tools.h"
#include "platform/platform.h"
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


// B6: a game that is gone is told as crashed or quit. Pure decision (judge), dump matching, the folders searched, the
// exit code read through a handle that outlives the process, and the words of the answer.
static crashinfo::Facts facts_ended(double exit_unix) {
    crashinfo::Facts f;
    f.start_unix = 1000;
    f.exit_known = true;
    f.exit_unix = exit_unix;
    f.last_seen_unix = exit_unix - 1;
    f.now_unix = exit_unix + 30;
    return f;
}

static void test_crash_report() {
    using crashinfo::Cause;
    using crashinfo::Dump;
    using crashinfo::Facts;
    std::set<std::string> none;
    std::vector<Dump> dumps{{"gmod_win64_a_1_accessviolation.mdmp", 5000.0}};

    // Exit codes: an NTSTATUS error is an unhandled exception.
    CHECK(crashinfo::is_exception_exit(0xC0000005u));
    CHECK(crashinfo::is_exception_exit(0xC0000094u));
    CHECK(!crashinfo::is_exception_exit(0));
    CHECK(!crashinfo::is_exception_exit(1));
    CHECK(!crashinfo::is_exception_exit(0x40010005u));  // Ctrl+C is not an error status

    // The decision. A dump written just before the end: crashed, whatever the exit code says.
    Facts f = facts_ended(5001.0);
    f.exit_code = 0xC0000005u;
    auto v = crashinfo::judge(f, dumps, none);
    CHECK(v.cause == Cause::Crashed && v.dump == dumps[0].name && !v.dump_ambiguous && !v.evidence.empty());
    f.exit_code = 0;  // Source's own crash handler may end the process normally after writing the dump
    CHECK(crashinfo::judge(f, dumps, none).cause == Cause::Crashed);
    // Asked to end (game_quit/game_terminate) or said goodbye: ended on purpose, even with a dump around and code 1.
    f.exit_code = 1;
    f.requested = true;
    v = crashinfo::judge(f, dumps, none);
    CHECK(v.cause == Cause::Quit && v.dump.empty());
    f.requested = false;
    f.goodbye = true;
    CHECK(crashinfo::judge(f, dumps, none).cause == Cause::Quit);
    f.goodbye = false;
    // Dumps of other processes: older than the process start, long before or after the end, already claimed.
    CHECK(crashinfo::judge(f, {{"x.mdmp", 900.0}}, none).cause == Cause::Crashed && crashinfo::judge(f, {{"x.mdmp", 900.0}}, none).dump.empty());  // exit 1, no dump: abnormal
    f.exit_code = 0;
    CHECK(crashinfo::judge(f, {{"x.mdmp", 900.0}}, none).cause == Cause::Quit);             // before the process even started
    CHECK(crashinfo::judge(f, {{"x.mdmp", 4900.0}}, none).cause == Cause::Quit);            // 100 s before it ended
    CHECK(crashinfo::judge(f, {{"x.mdmp", 5100.0}}, none).cause == Cause::Quit);            // long after
    CHECK(crashinfo::judge(f, dumps, std::set<std::string>{dumps[0].name}).cause == Cause::Quit);  // given to another instance
    // Exit code only.
    f.exit_code = 0;
    v = crashinfo::judge(f, {}, none);
    CHECK(v.cause == Cause::Quit && v.dump.empty());
    f.exit_code = 0xC0000094u;
    v = crashinfo::judge(f, {}, none);
    CHECK(v.cause == Cause::Crashed && v.dump.empty() && v.evidence.size() >= 1);
    f.exit_code = 1;
    CHECK(crashinfo::judge(f, {}, none).cause == Cause::Crashed);  // abnormal exit
    // Exit code unknown (no handle): dump near the last heartbeat, else unknown.
    Facts g;
    g.start_unix = 1000;
    g.last_seen_unix = 5000;
    g.now_unix = 5012;
    CHECK(crashinfo::judge(g, dumps, none).cause == Cause::Crashed);
    CHECK(crashinfo::judge(g, {}, none).cause == Cause::Unknown);
    CHECK(crashinfo::judge(g, {{"old.mdmp", 4000.0}}, none).cause == Cause::Unknown);
    g.goodbye = true;
    CHECK(crashinfo::judge(g, dumps, none).cause == Cause::Quit);
    // Start time unknown: no lower bound from it.
    Facts h = facts_ended(5001.0);
    h.start_unix = 0;
    h.exit_code = 0;
    CHECK(crashinfo::judge(h, dumps, none).cause == Cause::Crashed);

    // Matching: the closest to the end wins, ambiguity is reported, claimed dumps are skipped.
    Dump m;
    bool amb = false;
    std::vector<Dump> two{{"early.mdmp", 4990.0}, {"late.mdmp", 5000.0}};
    CHECK(crashinfo::match_dump(two, 4980, 5006, 5001, none, m, amb) && m.name == "late.mdmp" && amb);
    CHECK(crashinfo::match_dump(two, 4980, 5006, 5001, std::set<std::string>{"late.mdmp"}, m, amb) && m.name == "early.mdmp" && !amb);
    CHECK(!crashinfo::match_dump(two, 5100, 5200, 5150, none, m, amb));
    CHECK(!crashinfo::match_dump({}, 0, 1e12, 5, none, m, amb));

    // Where the dumps are: next to garrysmod/ (the game root), listed newest first, only .mdmp, missing folders fine.
    fs::path game = fs::temp_directory_path() / "gmod-mcp-coretests-crash" / "game";
    std::error_code ec;
    fs::remove_all(game.parent_path(), ec);
    fs::create_directories(game / "garrysmod");
    fs::create_directories(game / "crashes");
    auto write = [](const fs::path &p, double unix_time) {
        std::ofstream(p, std::ios::binary) << "MDMP";
        // file clock now vs system clock now gives the offset between the two epochs
        auto fnow = fs::file_time_type::clock::now();
        double snow = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        fs::last_write_time(p, fnow - std::chrono::duration_cast<fs::file_time_type::duration>(std::chrono::duration<double>(snow - unix_time)));
    };
    write(game / "crashes" / "gmod_win64_20261114_093156_1_accessviolation.mdmp", 1700000000.0);
    write(game / "crashes" / "gmod_win64_20261114_093200_1_crash.mdmp", 1700000100.0);
    write(game / "crashes" / "notes.txt", 1700000200.0);
    auto dirs = crashinfo::dump_dirs((game / "garrysmod").u8string());
    CHECK(!dirs.empty() && same(dirs[0], game / "crashes"));
    auto listed = crashinfo::list_dumps(dirs);
    CHECK(listed.size() == 2);
    CHECK(listed.size() == 2 && listed[0].name == "gmod_win64_20261114_093200_1_crash.mdmp" && std::abs(listed[0].mtime - 1700000100.0) < 2.0);
    CHECK(listed.size() == 2 && std::abs(listed[1].mtime - 1700000000.0) < 2.0);
    CHECK(crashinfo::list_dumps({(game / "nope").u8string()}).empty());
    CHECK(crashinfo::list_dumps({}).empty());
    fs::remove_all(game.parent_path(), ec);

    // The words of the answer: client or server, the dump, the exit code, relaunch for the client, never a "frozen" claim.
    CHECK(std::string(crashinfo::crash_code("client")) == "client_crashed");
    CHECK(std::string(crashinfo::crash_code("srcds")) == "server_crashed");
    crashinfo::Verdict cv;
    cv.cause = Cause::Crashed;
    cv.dump = "gmod_win64_20261114_093156_1_accessviolation.mdmp";
    cv.evidence = {"a crash dump was written"};
    std::string msg = crashinfo::crash_message("client", "client-9", 777, 4.0, cv, true, 0xC0000005u);
    CHECK(msg.find("client-9") != std::string::npos && msg.find("777") != std::string::npos);
    CHECK(msg.find(cv.dump) != std::string::npos && msg.find("crashes") != std::string::npos);
    CHECK(msg.find("instances_launch") != std::string::npos && msg.find("human") != std::string::npos);
    CHECK(msg.find("0xC0000005") != std::string::npos);
    cv.dump.clear();
    msg = crashinfo::crash_message("client", "client-9", 777, 4.0, cv, true, 1);
    CHECK(msg.find("No crash dump") != std::string::npos && msg.find("killed") != std::string::npos);
    msg = crashinfo::crash_message("srcds", "srv", 5, 1.0, cv, false, 0);
    CHECK(msg.find("server") != std::string::npos && msg.find("instances_launch") == std::string::npos);
    msg = crashinfo::quit_message("client-9", 777, 4.0, "it was closed on request");
    CHECK(msg.find("client-9") != std::string::npos && msg.find("closed on request") != std::string::npos && msg.find("crash") == std::string::npos);

    // The same dead-instance entry that instances_list shows.
    cv.dump = "d.mdmp";
    json e = crashinfo::crashed_entry("client-9", "client", 777, 4.0, cv, true, 0xC0000005u);
    CHECK(e.value("state", "") == "crashed" && e.value("id", "") == "client-9" && e.value("pid", 0) == 777);
    CHECK(e["crash"].value("dump", "") == "d.mdmp" && e["crash"].value("exit_code", 0u) == 0xC0000005u);
    CHECK(e["crash"]["evidence"].is_array());

#ifdef _WIN32
    // A handle opened while the process lives still reads its exit code and end time after it ended.
    {
        STARTUPINFOA si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        char cmd[] = "cmd.exe /c exit 3221225477";  // 0xC0000005
        bool started = CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CHECK(started);
        if (started) {
            void *w = plat::process_watch_open(pi.dwProcessId);
            WaitForSingleObject(pi.hProcess, 10000);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            plat::ProcInfo info;
            CHECK(w != nullptr && plat::process_watch_query(w, info, 5000));
            CHECK(info.exited && info.exit_code == 0xC0000005u);
            double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
            CHECK(info.start_unix > now - 60 && info.exit_unix >= info.start_unix && info.exit_unix < now + 5);
            plat::process_watch_close(w);
        }
        // A process that lives: not exited, the start time is known; waiting for it times out.
        PROCESS_INFORMATION pj{};
        char cmd2[] = "cmd.exe /c ping -n 30 127.0.0.1 >nul";
        if (CreateProcessA(nullptr, cmd2, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pj)) {
            void *w = plat::process_watch_open(pj.dwProcessId);
            plat::ProcInfo info;
            CHECK(w != nullptr && plat::process_watch_query(w, info, 100) && !info.exited && info.start_unix > 0);
            TerminateProcess(pj.hProcess, 1);
            WaitForSingleObject(pj.hProcess, 10000);
            CHECK(plat::process_watch_query(w, info, 0) && info.exited && info.exit_code == 1);  // killed from outside: code 1
            plat::process_watch_close(w);
            CloseHandle(pj.hThread);
            CloseHandle(pj.hProcess);
        }
    }
#endif
    CHECK(plat::process_watch_open(0) == nullptr);
}

#ifdef _WIN32
// B6 end to end in one process: this process is the coordinator, a real child process is the "instance" (its module is not
// there: calls to its port fail like they do for a game that died), and the answers to a call for it are checked after
// it was ended the ways a game ends: crash with a dump, crash without one, clean exit, asked to quit, said goodbye.
struct Child {
    PROCESS_INFORMATION pi{};
    bool start() {
        STARTUPINFOA si{};
        si.cb = sizeof si;
        char cmd[] = "cmd.exe /c ping -n 30 127.0.0.1 >nul";
        return CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != 0;
    }
    void end(UINT code) {
        TerminateProcess(pi.hProcess, code);
        WaitForSingleObject(pi.hProcess, 10000);
    }
    ~Child() {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
};

static void test_crash_report_coordinator() {
    tools::register_core();     // status
    tools::register_session();  // game_terminate
    auto &cfg = agent::config();
    std::string saved_token = cfg.token, saved_id = cfg.instance_id, saved_dir = cfg.game_dir;
    cfg.token = "coretests-token-0123456789abcdef";
    cfg.instance_id = "coordinator-test";
    int port = httpsrv::start(39500, 50, cfg.token);
    CHECK(port != 0);
    if (!port) return;
    cfg.base_port = port;
    cfg.port = port;
    coordinator::start();
    CHECK(coordinator::is_coordinator());

    fs::path game = fs::temp_directory_path() / "gmod-mcp-coretests-crash2" / "game";
    std::error_code ec;
    fs::remove_all(game.parent_path(), ec);
    fs::create_directories(game / "garrysmod");
    fs::create_directories(game / "crashes");
    auto write_dump = [&](const char *name) { std::ofstream(game / "crashes" / name, std::ios::binary) << "MDMP"; };
    auto heartbeat = [&](const std::string &id, const std::string &mode, DWORD pid, bool closing = false) {
        httplib::Client cli("127.0.0.1", port);
        cli.set_bearer_token_auth(cfg.token);
        json j{{"id", id}, {"port", 1}, {"pid", pid}, {"mode", mode}, {"game_dir", (game / "garrysmod").u8string()}, {"map", ""}};
        if (closing) j["closing"] = true;
        auto r = cli.Post("/internal/heartbeat", j.dump(), "application/json");
        return r && r->status == 200;
    };
    auto status_of = [&](const std::string &id, const char *tool = "status") {
        json r = mcp::call_tool(tool, json{{"instance", id}});
        return r.value("structuredContent", json::object()).value("error", json::object());
    };
    auto state_in_list = [&](const std::string &id) {
        for (const auto &e : coordinator::instances())
            if (e.value("id", "") == id) return e.value("state", "");
        return std::string("absent");
    };

    // 1. A crash with a dump written just before: client_crashed, the dump, the exit code; instances_list shows it as crashed.
    {
        Child c;
        CHECK(c.start());
        CHECK(heartbeat("crash-1", "client", c.pi.dwProcessId));
        CHECK(state_in_list("crash-1") == "running");
        write_dump("gmod_win64_20261114_101010_1_accessviolation.mdmp");
        c.end(0xC0000005u);
        json e = status_of("crash-1");
        CHECK(e.value("code", "") == "client_crashed");
        CHECK(e.value("message", "").find("gmod_win64_20261114_101010_1_accessviolation.mdmp") != std::string::npos);
        CHECK(e.value("details", json::object()).value("dump", "") == "gmod_win64_20261114_101010_1_accessviolation.mdmp");
        CHECK(e.value("details", json::object()).value("exit_code", 0u) == 0xC0000005u);
        CHECK(state_in_list("crash-1") == "crashed");
        // The same answer a second time (cached, the dump is not "used up").
        CHECK(status_of("crash-1").value("code", "") == "client_crashed");
        json entry;
        for (const auto &x : coordinator::instances())
            if (x.value("id", "") == "crash-1") entry = x;
        CHECK(entry["crash"].value("dump", "") == "gmod_win64_20261114_101010_1_accessviolation.mdmp" && entry.value("pid", 0u) == c.pi.dwProcessId);
    }
    // 2. Another crash, no new dump: the first crash's dump is not given again.
    {
        Child c;
        CHECK(c.start() && heartbeat("crash-2", "client", c.pi.dwProcessId));
        c.end(0xC0000005u);
        json e = status_of("crash-2");
        CHECK(e.value("code", "") == "client_crashed" && e["details"]["dump"].is_null());
    }
    // 3. A dedicated server that crashed: server_crashed.
    {
        Child c;
        CHECK(c.start() && heartbeat("srv-1", "srcds", c.pi.dwProcessId));
        c.end(0xC0000094u);
        CHECK(status_of("srv-1").value("code", "") == "server_crashed");
    }
    // 4. A clean exit (code 0): instance_gone, not a crash, not in the list as crashed.
    {
        Child c;
        CHECK(c.start() && heartbeat("quit-1", "client", c.pi.dwProcessId));
        c.end(0);
        json e = status_of("quit-1");
        CHECK(e.value("code", "") == "instance_gone" && e.value("message", "").find("crash") == std::string::npos);
        CHECK(state_in_list("quit-1") == "absent");
    }
    // 5. Said goodbye (its module closed with the game), then the process ended with a code that would otherwise look abnormal.
    {
        Child c;
        CHECK(c.start() && heartbeat("bye-1", "client", c.pi.dwProcessId));
        write_dump("gmod_win64_20261114_101111_1_crash.mdmp");  // another game's dump around the same time
        CHECK(heartbeat("bye-1", "client", c.pi.dwProcessId, true));
        c.end(1);
        json e = status_of("bye-1");
        CHECK(e.value("code", "") == "instance_gone" && e.value("message", "").find("crash") == std::string::npos);
    }
    // 6. Asked to end through the coordinator (game_terminate relayed to it): ended on request, not a crash. The call itself
    // fails (nothing listens on the fake port) and the child is still alive then: that is instance_unreachable.
    {
        Child c;
        CHECK(c.start() && heartbeat("req-1", "client", c.pi.dwProcessId));
        json e = status_of("req-1", "game_terminate");
        CHECK(e.value("code", "") == "instance_unreachable");
        c.end(1);
        e = status_of("req-1");
        CHECK(e.value("code", "") == "instance_gone" && e.value("message", "").find("asked to end") != std::string::npos);
    }
    // 7. The process dies while a call to it is in flight (the connection breaks first, the process object a moment later).
    {
        Child c;
        CHECK(c.start() && heartbeat("flight-1", "client", c.pi.dwProcessId));
        write_dump("gmod_win64_20261114_101212_1_accessviolation.mdmp");
        std::thread killer([&] {
            plat::sleep_ms(400);
            c.end(0xC0000005u);
        });
        json e = status_of("flight-1");  // the call fails at once, then waits for the process to end
        killer.join();
        CHECK(e.value("code", "") == "client_crashed");
        CHECK(e.value("message", "").find("gmod_win64_20261114_101212_1_accessviolation.mdmp") != std::string::npos);
    }
    // 8. Alive but not answering: unchanged (instance_unreachable, "still running").
    {
        Child c;
        CHECK(c.start() && heartbeat("hung-1", "client", c.pi.dwProcessId));
        json e = status_of("hung-1");
        CHECK(e.value("code", "") == "instance_unreachable" && e.value("message", "").find("still running") != std::string::npos);
        c.end(1);
    }
    // 9. An id that never existed.
    CHECK(status_of("never-1").value("code", "") == "unknown_instance");

    coordinator::stop();
    httpsrv::stop();
    cfg.token = saved_token;
    cfg.instance_id = saved_id;
    cfg.game_dir = saved_dir;
    fs::remove_all(game.parent_path(), ec);
}
#endif


// menu.lua loader block: install then remove must give back the original bytes, whatever the original ends with.
static void test_installer_roundtrip() {
    fs::path base = fs::temp_directory_path() / "gmod-mcp-coretests-installer";
    std::error_code ec;
    fs::remove_all(base, ec);
    struct Case { const char *name; std::string content; };
    const Case cases[] = {
        {"lf_trailing", "line1\nline2\n"},
        {"crlf_trailing", "line1\r\nline2\r\n"},
        {"lf_no_trailing", "line1\nline2"},
        {"crlf_no_trailing", "a\r\nb"},
        {"empty", ""},
        {"one_char", "x"},
        {"blank_lines_end", "x\n\n\n"},
        {"only_newline", "\n"},
        {"mixed_no_trailing", "a\r\nb\nc"},
    };
    for (const auto &c : cases) {
        std::string g = (base / c.name).u8string();
        fs::create_directories(base / c.name / "lua" / "menu");
        std::string menu = g + "/lua/menu/menu.lua", err, got;
        CHECK(plat::write_file(menu, c.content));
        CHECK(installer::ensure_menu_patch(g, err) == "installed");
        plat::read_file(menu, got);
        CHECK(got.find("-- MCPAGENT BEGIN") != std::string::npos);
        CHECK(installer::ensure_menu_patch(g, err) == "unchanged");
        CHECK(installer::remove_menu_patch(g, err));
        plat::read_file(menu, got);
        if (got != c.content) std::printf("  roundtrip differs for %s\n", c.name);
        CHECK(got == c.content);
        // a second cycle after the removal gives the same result (the backup is already there)
        CHECK(installer::ensure_menu_patch(g, err) == "installed");
        CHECK(installer::remove_menu_patch(g, err));
        plat::read_file(menu, got);
        CHECK(got == c.content);
    }
    // An install written by an older version (blank line before the block, even when the original had no final
    // newline): the backup tells what the original was.
    {
        std::string g = (base / "legacy").u8string();
        fs::create_directories(base / "legacy" / "lua" / "menu");
        std::string menu = g + "/lua/menu/menu.lua", err, got, installed;
        plat::write_file(menu, "a\nb");
        installer::ensure_menu_patch(g, err);
        plat::read_file(menu, installed);
        size_t b = installed.find("-- MCPAGENT BEGIN");
        plat::write_file(menu, installed.substr(0, b) + "\n" + installed.substr(b));  // old layout: one more line break
        CHECK(installer::remove_menu_patch(g, err));
        plat::read_file(menu, got);
        CHECK(got == "a\nb");
    }
    fs::remove_all(base, ec);
}

// Issue #3: the guide documents where the hints of a result live (the model of some clients reads structuredContent on
// success and only the text on an error), so the conventions are written down and cannot silently drift.
static void test_guide_result_conventions() {
    json g = resources::read("mcpagent://guide");
    std::string text = g.is_object() && g.contains("text") ? g["text"].get<std::string>() : "";
    CHECK(text.find("## Reading results") != std::string::npos);
    for (const char *field : {"structuredContent", "isError", "note", "warning", "next", "truncated_note", "fps_note",
                              "output_note", "version_warning", "waited_s", "game_not_running", "answered_by", "next_call",
                              "after_s", "outputSchema"})
        CHECK(text.find(field) != std::string::npos);
}

// Issue #3: every tool declares a real outputSchema, and representative success results (tests/schema/results.json)
// match it. A client that validates results (the MCP SDK in Claude Code) fails the call on a mismatch, so a schema that
// drifts from what the code returns must fail here first. isError results are not validated by clients: not covered.
static void test_output_schemas() {
    if (mcp::tools().empty()) tools::register_all();
    std::ifstream in(fs::u8path(std::string(GMOD_MCP_SOURCE_DIR) + "/tests/schema/results.json"), std::ios::binary);
    CHECK(in.good());
    json fx = json::parse(in, nullptr, false);
    CHECK(fx.is_object());
    if (!fx.is_object()) return;
    std::set<std::string> names;
    for (const auto &t : mcp::tools()) {
        names.insert(t.name);
        json declared = mcp::output_schema_for(t.name);
        CHECK(!declared.is_null());                // every tool has its own schema, not the permissive default
        CHECK(t.output_schema == declared);        // and registers with it (tools/list sends it)
        bool has_cases = fx.contains(t.name) && fx[t.name].is_array() && !fx[t.name].empty();
        if (!has_cases) std::printf("no result fixture for tool %s\n", t.name.c_str());
        CHECK(has_cases);
        if (!has_cases) continue;
        nlohmann::json_schema::json_validator v;
        v.set_root_schema(t.output_schema);
        for (const auto &c : fx[t.name]) {
            bool matches = true;
            std::string why;
            try {
                v.validate(c["result"]);
            } catch (const std::exception &e) {
                matches = false;
                why = e.what();
            }
            if (!matches) std::printf("%s (%s) does not match its outputSchema: %s\n", t.name.c_str(), c.value("case", "?").c_str(), why.c_str());
            CHECK(matches);
        }
    }
    for (auto it = fx.begin(); it != fx.end(); ++it)
        if (it.key() != "_comment" && !names.count(it.key())) {
            std::printf("fixture for unknown tool %s\n", it.key().c_str());
            CHECK(false);
        }
    // The validator really enforces the schemas: wrong types, a missing required field and a wrong nested type fail.
    auto fails = [&](const char *tool, const json &bad) {
        const mcp::Tool *t = mcp::find_tool(tool);
        if (!t) return false;
        nlohmann::json_schema::json_validator v;
        v.set_root_schema(t->output_schema);
        try {
            v.validate(bad);
        } catch (const std::exception &) {
            return true;
        }
        return false;
    };
    CHECK(fails("ents_find", json{{"entities", "x"}, {"count", 0}, {"truncated", false}}));              // array expected
    CHECK(fails("ents_find", json{{"entities", json::array()}, {"truncated", false}}));                  // count required
    CHECK(fails("ents_find", json{{"entities", json::array({json{{"id", "7"}}})}, {"count", 1}, {"truncated", false}}));  // id is a number
    CHECK(fails("map_list", json{{"maps", json::array({1})}, {"count", 1}, {"truncated", false}}));
    CHECK(fails("wait_frames", json{{"frames", "20"}, {"elapsed_s", 1}}));
    CHECK(fails("status", json{{"main_thread_stalled", "no"}}));
    // Extra fields stay allowed (a newer module may add some), and the hint fields are strings.
    const mcp::Tool *wf = mcp::find_tool("wait_frames");
    CHECK(wf != nullptr);
    if (wf) {
        nlohmann::json_schema::json_validator v;
        v.set_root_schema(wf->output_schema);
        bool ok_extra = true;
        try {
            v.validate(json{{"frames", 1}, {"elapsed_s", 0.1}, {"future_field", {{"a", 1}}}, {"note", "n"}});
        } catch (const std::exception &) {
            ok_extra = false;
        }
        CHECK(ok_extra);
    }
}

// Issue #3: the hints that name a next call also give it as data (`next_call`), next to the prose. Each case is a result
// as the module builds it; the expected next_call is what the eval in tests/eval/hint_eval.py showed the model.
static json call_result(json sc, bool is_error) {
    return json{{"content", json::array({json{{"type", "text"}, {"text", sc.dump()}}})}, {"structuredContent", std::move(sc)}, {"isError", is_error}};
}
static json err_result(const std::string &code, const std::string &message, json details = nullptr) {
    json e{{"code", code}, {"message", message}};
    if (!details.is_null()) e["details"] = std::move(details);
    return call_result(json{{"error", e}, {"answered_by", "client-1"}}, true);
}
static json next_of(json r, const std::string &tool, const json &args) {
    next_call::annotate(r, tool, args);
    const json &sc = r["structuredContent"];
    bool text_ok = json::parse(r["content"][0]["text"].get<std::string>()) == sc;  // text stays the JSON of structuredContent
    CHECK(text_ok);
    if (r.value("isError", false)) return sc["error"].value("next_call", json());
    return sc.value("next_call", json());
}
static void test_next_call() {
    // A realm that does not exist yet: load a map (the call's instance is kept).
    CHECK(next_of(err_result("realm_unavailable", "realm server unavailable (not loaded)"), "players_list", json{{"realm", "server"}}) ==
          (json{{"tool", "map_load"}}));
    CHECK(next_of(err_result("realm_unavailable", "realm client unavailable"), "ents_find", json{{"instance", "client-2"}}) ==
          (json{{"tool", "map_load"}, {"arguments", {{"instance", "client-2"}}}}));
    // The main thread did not answer: look at the status.
    CHECK(next_of(err_result("main_thread_timeout", "x", json{{"timeout_s", 60}}), "lua_exec", json{{"code", "x"}}) == (json{{"tool", "status"}}));
    // Several instances share the endpoint.
    CHECK(next_of(err_result("ambiguous_instance", "x", json{{"instances", json::array({"a (this one)", "b"})}}), "lua_exec", json::object()) ==
          (json{{"tool", "instances_list"}}));
    // A game crashed or ended: relaunch a client (not a dedicated server).
    CHECK(next_of(err_result("client_crashed", "x", json{{"instance", "client-2"}}), "tests_run", json{{"instance", "client-2"}}) ==
          (json{{"tool", "instances_launch"}, {"arguments", {{"count", 1}}}}));
    CHECK(next_of(err_result("instance_gone", "x"), "status", json{{"instance", "client-2"}}) == (json{{"tool", "instances_launch"}, {"arguments", {{"count", 1}}}}));
    CHECK(next_of(err_result("server_crashed", "x"), "status", json{{"instance", "srv"}}).is_null());
    // A game that does not answer right after a launch: the same status, a little later.
    CHECK(next_of(err_result("instance_unreachable", "x"), "status", json{{"instance", "client-2"}}) ==
          (json{{"tool", "status"}, {"arguments", {{"instance", "client-2"}}}, {"after_s", 10}}));
    // A loop that used its budget: the same code as a job.
    CHECK(next_of(err_result("budget_exceeded", "x"), "lua_exec", json{{"code", "return 1"}, {"realm", "server"}, {"budget", 0.5}}) ==
          (json{{"tool", "lua_job"}, {"arguments", {{"code", "return 1"}, {"realm", "server"}, {"async", true}}}}));
    // Errors with no obvious next step get nothing, and a success without a hint is untouched.
    CHECK(next_of(err_result("not_found", "x"), "lua_file_read", json::object()).is_null());
    CHECK(next_of(call_result(json{{"values", json::array()}, {"n", 0}}, false), "lua_exec", json::object()).is_null());
    // A started job: poll it.
    CHECK(next_of(call_result(json{{"job_id", "server-g3-1"}, {"state", "running"}, {"realm", "server"}}, false), "lua_job", json{{"async", true}}) ==
          (json{{"tool", "lua_job_status"}, {"arguments", {{"id", "server-g3-1"}, {"wait_s", 30}}}}));
    CHECK(next_of(call_result(json{{"job_id", "server-g3-1"}, {"state", "running"}}, false), "lua_job", json{{"async", true}, {"instance", "c2"}}) ==
          (json{{"tool", "lua_job_status"}, {"arguments", {{"id", "server-g3-1"}, {"wait_s", 30}, {"instance", "c2"}}}}));
    // A finished synchronous job (no state running) gets nothing.
    CHECK(next_of(call_result(json{{"job_id", "server-g3-1"}, {"values", json::array()}, {"n", 0}}, false), "lua_job", json::object()).is_null());
    // Cut values: the same call without the limits that cut them.
    CHECK(next_of(call_result(json{{"values", json::array({1})}, {"n", 1}, {"truncated", 2}, {"truncated_note", "x"}}, false), "lua_exec",
                  json{{"code", "return t"}, {"realm", "server"}, {"limit", 50}, {"depth", 2}, {"max_string", 100}}) ==
          (json{{"tool", "lua_exec"}, {"arguments", {{"code", "return t"}, {"realm", "server"}}}}));
    // An unknown argument with a close accepted name: the corrected call, through the real validation (no game needed).
    if (mcp::tools().empty()) tools::register_all();
    json r = mcp::call_tool("console_exec", json{{"command", "echo hi"}});
    CHECK(r.value("isError", false) && r["structuredContent"]["error"]["code"] == "invalid_params");
    CHECK(r["structuredContent"]["error"].value("next_call", json()) == (json{{"tool", "console_exec"}, {"arguments", {{"cmd", "echo hi"}}}}));
    CHECK(json::parse(r["content"][0]["text"].get<std::string>()) == r["structuredContent"]);
    // Not offered when the corrected call would still be invalid (a missing required argument).
    json r2 = mcp::call_tool("console_exec", json{{"comand", "echo hi"}, {"realm", "nowhere"}});
    CHECK(r2["structuredContent"]["error"].value("next_call", json()).is_null());
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // failures stay visible even if a test crashes the process (CI pipes stdout)
    test_installer_roundtrip();
    test_guide_result_conventions();
    test_output_schemas();
    test_next_call();
    test_cmdline();
    test_crash_report();
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
#ifdef _WIN32
    test_crash_report_coordinator();
#endif
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

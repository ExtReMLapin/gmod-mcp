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
#include <future>
#include <string>
#include <set>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

#include "agent.h"
#include "argcheck.h"
#include "cmdline.h"
#include "gamedir.h"
#include "console.h"
#include "coordinator.h"
#include "gmod_mcp_reply.h"
#include "http_server.h"
#include "installer.h"
#include "launch_opts.h"
#include "crashinfo.h"
#include "health.h"
#include "jobs.h"
#include "lowmem.h"
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

// Issue #11: errors_since {group_by: "message"} returns one entry per distinct message (count, first and last occurrence,
// most frequent first) instead of up to 2000 mostly identical errors; without group_by the output is unchanged.
static void test_errors_group_by_message() {
    console::errors_clear();
    agent::note_map_load_start("gm_g");
    console::push_error(json{{"realm", "server"}, {"message", "rare"}, {"time", 1.0}});
    for (int i = 0; i < 5; ++i) console::push_error(json{{"realm", i == 4 ? "client" : "server"}, {"message", "spam"}, {"time", 2.0 + i}});
    console::push_error(json{{"realm", "server"}, {"message", "twice"}, {"time", 10.0}});
    console::push_error(json{{"realm", "server"}, {"message", "twice"}, {"time", 11.0}});
    json plain = mcp::call_tool("errors_since", json{{"cursor", 0}}).value("structuredContent", json::object());
    CHECK(plain["errors"].size() == 8 && !plain.contains("groups"));  // default output unchanged
    json r = mcp::call_tool("errors_since", json{{"cursor", 0}, {"group_by", "message"}});
    CHECK(!r.value("isError", true));
    json sc = r.value("structuredContent", json::object());
    CHECK(sc.contains("groups") && sc["groups"].is_array() && sc["groups"].size() == 3);
    if (!(sc.contains("groups") && sc["groups"].size() == 3)) return;
    const json &g0 = sc["groups"][0], &g1 = sc["groups"][1], &g2 = sc["groups"][2];
    CHECK(g0.value("message", "") == "spam" && g0.value("count", 0) == 5);  // most frequent first
    CHECK(g1.value("message", "") == "twice" && g1.value("count", 0) == 2);
    CHECK(g2.value("message", "") == "rare" && g2.value("count", 0) == 1);
    CHECK(g0.value("last_seq", 0) == g0.value("first_seq", 0) + 4);  // seq of first and last occurrence
    CHECK(g0.value("first_time", 0.0) == 2.0 && g0.value("last_time", 0.0) == 6.0);
    CHECK(g1.value("first_time", 0.0) == 10.0 && g1.value("last_time", 0.0) == 11.0);
    CHECK(g2.value("first_seq", 1) == g2.value("last_seq", 2) && g2.value("count", 0) == 1);
    CHECK(g0.contains("realms") && g0["realms"].size() == 2 && g1["realms"].size() == 1);
    CHECK(g0.value("map", "") == "gm_g");
    CHECK(sc.value("next_cursor", 0) == plain.value("next_cursor", -1) && sc.contains("map_load"));
    // The filters still apply before grouping, and `limit` caps the groups (the cut is reported).
    json rc = mcp::call_tool("errors_since", json{{"cursor", 0}, {"group_by", "message"}, {"realm", "client"}}).value("structuredContent", json::object());
    CHECK(rc["groups"].size() == 1 && rc["groups"][0].value("count", 0) == 1);
    json rl = mcp::call_tool("errors_since", json{{"cursor", 0}, {"group_by", "message"}, {"limit", 2}}).value("structuredContent", json::object());
    CHECK(rl["groups"].size() == 2 && rl.value("groups_total", 0) == 3);
    // Any other group_by value is refused by the schema.
    json bad = mcp::call_tool("errors_since", json{{"cursor", 0}, {"group_by", "realm"}});
    CHECK(bad.value("isError", false));
}

// Issue #11: `fields` on a result that holds no table (a JSON string from util.TableToJSON, a number) projects nothing: the
// result must say so (`fields_note`) instead of silently coming back whole.
static void test_fields_note() {
    json args{{"fields", {"pass", "fails"}}};
    auto run = [&](json values) { return tools::project_values(tools::ok(json{{"values", std::move(values)}, {"n", 1}}), args); };
    json str = run(json::array({"{\"pass\":true,\"fails\":0}"}));
    CHECK(str["result"].contains("fields_note"));
    CHECK(str["result"].value("fields_note", "").find("string") != std::string::npos);
    CHECK(str["result"].value("fields_note", "").find("JSONToTable") != std::string::npos);
    CHECK(str["result"]["values"][0].is_string());  // the value itself is untouched
    CHECK(run(json::array({1, 2}))["result"].contains("fields_note"));
    CHECK(run(json::array())["result"].contains("fields_note"));
    CHECK(run(json::array({json::array({1})}))["result"].contains("fields_note"));  // an array is not a table of keys
    // A table was projected: no note. A mix: projected, no note.
    json tbl = run(json::array({json{{"pass", true}, {"fails", 0}, {"big", "x"}}}));
    CHECK(!tbl["result"].contains("fields_note") && tbl["result"]["values"][0].size() == 2);
    CHECK(!run(json::array({"s", json{{"pass", true}}}))["result"].contains("fields_note"));
    // No `fields`: nothing added.
    json plain = tools::project_values(tools::ok(json{{"values", json::array({"s"})}, {"n", 1}}), json::object());
    CHECK(!plain["result"].contains("fields_note") && !plain["result"].contains("projected"));
}

// Issue #11: the long waits accept up to an hour (a long series no longer needs chained calls), and nothing on the way
// cuts a call that long: the coordinator's relay waits for an instance's answer longer than the longest wait.
static void test_long_waits() {
    if (mcp::tools().empty()) tools::register_all();
    struct Cap { const char *tool, *arg; };
    for (Cap c : {Cap{"wait_for", "timeout_s"}, Cap{"wait_frames", "timeout_s"}, Cap{"events_wait", "timeout_s"}, Cap{"reports_read", "wait_s"},
                  Cap{"console_wait", "timeout_s"}, Cap{"lua_job_status", "wait_s"}}) {
        const mcp::Tool *found = nullptr;
        for (const auto &t : mcp::tools())
            if (t.name == c.tool) found = &t;
        CHECK(found != nullptr);
        if (!found) continue;
        double max = found->input_schema["properties"][c.arg].value("maximum", 0.0);
        if (max != 3600) std::printf("%s.%s maximum is %g\n", c.tool, c.arg, max);
        CHECK(max == 3600);
    }
    // Above the cap the schema refuses the call.
    json too_long = mcp::call_tool("wait_for", json{{"condition", "true"}, {"timeout_s", 3601}});
    CHECK(too_long.value("isError", false));
    CHECK(coordinator::kRelayReadTimeoutS >= 3600 + 60);
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

// Issue #4: a PE file with just enough header for pe_machine (DOS header, e_lfanew = 0x80, "PE\0\0", machine).
static void write_pe(const fs::path &p, uint16_t machine) {
    fs::create_directories(p.parent_path());
    std::string b(0x100, '\0');
    b[0] = 'M';
    b[1] = 'Z';
    b[0x3C] = (char)0x80;
    b.replace(0x80, 4, std::string("PE\0\0", 4));
    b[0x84] = (char)(machine & 0xFF);
    b[0x85] = (char)(machine >> 8);
    std::ofstream(p, std::ios::binary) << b;
}

// Issue #10: instances_launch {fps_max, fps_max_nofocus, commands, launch_args}: what is accepted and what is refused, before
// anything starts.
static void test_launch_options() {
    using launchopts::check_commands;
    using launchopts::check_launch_args;
    using launchopts::fps_error;
    // fps_max / fps_max_nofocus: an integer, 0 (no cap) or 10..1000.
    CHECK(fps_error(json(0), "fps_max").empty());
    CHECK(fps_error(json(10), "fps_max").empty());
    CHECK(fps_error(json(300), "fps_max").empty());
    CHECK(fps_error(json(1000), "fps_max_nofocus").empty());
    CHECK(fps_error(json(5), "fps_max").find("fps_max") != std::string::npos);
    CHECK(!fps_error(json(9), "fps_max").empty());
    CHECK(!fps_error(json(1001), "fps_max_nofocus").empty() && fps_error(json(1001), "fps_max_nofocus").find("fps_max_nofocus") != std::string::npos);
    CHECK(!fps_error(json(-1), "fps_max").empty());
    CHECK(!fps_error(json(30.5), "fps_max").empty());
    CHECK(!fps_error(json("60"), "fps_max").empty());
    CHECK(fps_error(json(60.0), "fps_max").empty());  // 60.0 is an integer in JSON

    // commands: console statements run once the client answers.
    std::vector<std::string> st;
    CHECK(check_commands(json::array({"fps_max 300", "snd_mute_losefocus 0"}), st).empty() && st.size() == 2 && st[0] == "fps_max 300");
    CHECK(check_commands(json::array(), st).empty() && st.empty());
    CHECK(check_commands(json::array({"cl_showfps 1; mat_vsync 0", "echo \"a;b\""}), st).empty() && st.size() == 3 && st[1] == "mat_vsync 0" && st[2] == "echo \"a;b\"");  // split like the engine does
    for (const char *bad : {"quit", "QUIT", "  exit ", "echo hi; quit", "connect 192.168.1.5", "connect 127.0.0.1", "retry", "redirect x",
                            "connect_lobby 1", "map gm_construct", "changelevel gm_flatgrass", "load save1", "host_workshop_map 1", "map_background x"}) {
        std::string e = check_commands(json::array({bad}), st);
        CHECK(!e.empty() && e.find("commands") != std::string::npos);
    }
    CHECK(check_commands(json::array({"map gm_construct"}), st).find("map option") != std::string::npos);  // says what to use instead
    CHECK(check_commands(json::array({"quit"}), st).find("game_quit") != std::string::npos || check_commands(json::array({"quit"}), st).find("instances_close") != std::string::npos);
    CHECK(check_commands(json::array({"fps_max 300", "quit"}), st).find("\"quit\"") != std::string::npos);  // names the refused command
    CHECK(!check_commands(json::array({""}), st).empty() && !check_commands(json::array({"   "}), st).empty());
    CHECK(!check_commands(json::array({42}), st).empty() && !check_commands(json("fps_max 300"), st).empty());
    CHECK(!check_commands(json::array({std::string(300, 'a')}), st).empty());
    CHECK(!check_commands(json::array({"echo \x01"}), st).empty());
    json many = json::array();
    for (int i = 0; i < 33; ++i) many.push_back("echo x");
    CHECK(!check_commands(many, st).empty());

    // launch_args: one token per element; what gmod-mcp controls and anything that connects is refused.
    std::vector<std::string> tk;
    CHECK(check_launch_args(json::array({"-noborder", "-nosound", "+cl_showfps", "1"}), tk).empty() && tk.size() == 4 && tk[2] == "+cl_showfps");
    CHECK(check_launch_args(json::array(), tk).empty() && tk.empty());
    for (const char *bad : {"-multirun", "-MULTIRUN", "-mcp", "-mcpport", "-mcp_test_client", "-mcp_lua_mount", "-mcp_restore_convars", "+mcp_x", "-w", "-h", "-width", "-height",
                            "-windowed", "-window", "-sw", "-fullscreen", "-full", "-dxlevel", "-condebug", "-game", "+map", "+changelevel",
                            "+connect", "-connect", "+connect_lobby", "+retry", "+redirect", "+quit", "+exit", "+load", "+host_workshop_map"}) {
        std::string e = check_launch_args(json::array({"-noborder", bad}), tk);
        CHECK(!e.empty() && e.find("launch_args") != std::string::npos && e.find(bad) != std::string::npos);  // names the refused option
    }
    CHECK(check_launch_args(json::array({"+map"}), tk).find("map option") != std::string::npos);
    CHECK(check_launch_args(json::array({"-w"}), tk).find("640x360") != std::string::npos || check_launch_args(json::array({"-w"}), tk).find("window") != std::string::npos);
    CHECK(check_launch_args(json::array({"-nosound"}), tk).empty());  // same as sound: false
    // Tokens that could end the argument or start another command are refused.
    for (const char *bad : {"", " ", "-a b", "-a\"b", "-a;b", "-a\nb", "-a\tb", "+echo \"x\"", "caf\xC3\xA9"})
        CHECK(!check_launch_args(json::array({bad}), tk).empty());
    CHECK(!check_launch_args(json::array({std::string(200, 'a')}), tk).empty());
    CHECK(!check_launch_args(json("-nosound"), tk).empty() && !check_launch_args(json::array({1}), tk).empty());
    json many_args = json::array();
    for (int i = 0; i < 33; ++i) many_args.push_back("-x");
    CHECK(!check_launch_args(many_args, tk).empty());

    // Convars the command line and the commands can change (the module gives them back before a clean quit).
    auto names = launchopts::restore_names({"fps_max 300", "toggle snd_mute_losefocus", "echo hi", "FPS_MAX 100", "bad;name 1", "incrementvar cl_showfps 0 3 1", "mat_vsync"},
                                           {"-noborder", "+cl_drawhud", "0", "+Mat_Antialias", "4", "+cl_showfps", "1"});
    auto has = [&](const char *n) { return std::find(names.begin(), names.end(), n) != names.end(); };
    CHECK(has("fps_max") && has("snd_mute_losefocus") && has("cl_showfps") && has("cl_drawhud") && has("mat_antialias") && has("mat_vsync") && has("echo"));
    CHECK(std::count(names.begin(), names.end(), "fps_max") == 1 && !has("toggle") && !has("incrementvar") && !has("noborder") && !has("0"));
    CHECK(!has("bad;name") && names.size() == 7);
    CHECK(launchopts::restore_names({}, {}).empty());
    // The module reads the list from -mcp_restore_convars.
    auto parsed = launchopts::parse_restore_list("cl_showfps,FPS_MAX,bad name,mat_vsync,,cl_showfps,a;b");
    CHECK(parsed == std::vector<std::string>({"cl_showfps", "fps_max", "mat_vsync"}));
    CHECK(launchopts::parse_restore_list("").empty());
    CHECK(launchopts::join_list({"a", "b"}) == "a,b" && launchopts::join_list({}).empty());
}

// Issue #4: the current x86-64 install has bin/win64/gmod.exe and no gmod_win64.exe (its root gmod.exe is 32-bit).
static void test_game_exe_layouts() {
    fs::path base = fs::temp_directory_path() / "gmod-mcp-coretests-gamedir";
    std::error_code ec;
    fs::remove_all(base, ec);
    auto exe_of = [&](const char *name) { return gamedir::find_game_exe((base / name).u8string()); };
    auto is = [&](const std::string &found, const fs::path &want) { return !found.empty() && same(found, want); };

    write_pe(base / "old/gmod_win64.exe", gamedir::kMachineX64);
    CHECK(gamedir::pe_machine((base / "old/gmod_win64.exe").u8string()) == gamedir::kMachineX64);  // Issue #4
    CHECK(is(exe_of("old"), base / "old/gmod_win64.exe"));  // Issue #4: the older layout still works

    write_pe(base / "cur/gmod.exe", gamedir::kMachineX86);  // the 32-bit game at the root, never taken
    write_pe(base / "cur/bin/win64/gmod.exe", gamedir::kMachineX64);
    CHECK(gamedir::pe_machine((base / "cur/gmod.exe").u8string()) == gamedir::kMachineX86);  // Issue #4
    CHECK(is(exe_of("cur"), base / "cur/bin/win64/gmod.exe"));  // Issue #4: the current layout is found

    write_pe(base / "both/gmod_win64.exe", gamedir::kMachineX64);
    write_pe(base / "both/bin/win64/gmod.exe", gamedir::kMachineX64);
    CHECK(is(exe_of("both"), base / "both/gmod_win64.exe"));  // Issue #4: the root name stays first when both are 64-bit

    write_pe(base / "wrong/gmod_win64.exe", gamedir::kMachineX86);
    write_pe(base / "wrong/bin/win64/gmod.exe", gamedir::kMachineX64);
    CHECK(is(exe_of("wrong"), base / "wrong/bin/win64/gmod.exe"));  // Issue #4: the machine type decides, not the name

    write_pe(base / "only32/gmod.exe", gamedir::kMachineX86);
    CHECK(exe_of("only32").empty());  // Issue #4: a 32-bit install has no 64-bit game
    write_pe(base / "only32b/bin/win64/gmod.exe", gamedir::kMachineX86);
    CHECK(exe_of("only32b").empty());  // Issue #4

    touch(base / "plain/bin/win64/gmod.exe");  // not a PE image: kept when nothing better exists (tests use stand-ins)
    CHECK(gamedir::pe_machine((base / "plain/bin/win64/gmod.exe").u8string()) == 0);  // Issue #4
    CHECK(is(exe_of("plain"), base / "plain/bin/win64/gmod.exe"));  // Issue #4
    touch(base / "mix/gmod_win64.exe");
    write_pe(base / "mix/bin/win64/gmod.exe", gamedir::kMachineX64);
    CHECK(is(exe_of("mix"), base / "mix/bin/win64/gmod.exe"));  // Issue #4: a real 64-bit image beats a file of unknown type

    fs::create_directories(base / "empty");
    CHECK(exe_of("empty").empty() && exe_of("missing").empty() && gamedir::find_game_exe("").empty());  // Issue #4
    CHECK(gamedir::pe_machine((base / "missing/x.exe").u8string()) == 0);  // Issue #4
    std::string msg = gamedir::missing_exe_message((base / "empty").u8string());
    CHECK(msg.find("gmod_win64.exe") != std::string::npos && msg.find("bin") != std::string::npos &&
          msg.find("win64") != std::string::npos && msg.find("gmod.exe") != std::string::npos);  // Issue #4: names both paths
    msg = gamedir::missing_exe_message((base / "only32").u8string());
    CHECK(msg.find("32-bit") != std::string::npos);  // Issue #4: says what it saw
    fs::remove_all(base, ec);
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

// Issue #9: the GMod x86-64 build of 2026-11-19 grew IAppSystem from 5 to 9 slots, so every IFileSystem slot moved by
// 4: slot 7 (AddSearchPath before) became a bare `ret 0` and slot 13 (GetSearchPath before) RemoveAllSearchPaths. The
// module now reads the layout from the code of slots 7, 8, 11 and 12. Bytes copied from both filesystem_stdio.dll
// builds (docs/REVERSE.md, "Lua mount: search paths").
static void test_fs_vtable_layout() {
    using B = std::vector<uint8_t>;
    // Old build (5 IAppSystem slots): 7 AddSearchPath (thunk with a body), 8 RemoveSearchPath; 11/12 do not matter.
    B old7{0x48, 0x83, 0xec, 0x38, 0xc6, 0x44, 0x24, 0x28, 0x00, 0xc6, 0x44, 0x24, 0x20, 0x01, 0xe8, 0x0d};
    B old8{0x48, 0x89, 0x5c, 0x24, 0x20, 0x56, 0x57, 0x41, 0x56, 0x48, 0x81, 0xec, 0x30, 0x01, 0x00, 0x00};
    // 2026-11-19 build (9 IAppSystem slots): 7 Reconnect `ret 0`, 8 IsSingleton `mov al,1; ret`, 11 AddSearchPath,
    // 12 RemoveSearchPath.
    B new7{0xc2, 0x00, 0x00, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc};
    B new8{0xb0, 0x01, 0xc3, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc};
    B new11{0x48, 0x83, 0xec, 0x38, 0xc6, 0x44, 0x24, 0x28, 0x00, 0xc6, 0x44, 0x24, 0x20, 0x01, 0xe8, 0x0d};
    B new12{0x40, 0x53, 0x56, 0x57, 0x41, 0x56, 0x48, 0x81, 0xec, 0x58, 0x01, 0x00, 0x00, 0x48, 0x8b, 0x05};
    CHECK(lua_mount::fs_slot_base(old7, old8, new11, new12) == 5);  // Issue #9
    CHECK(lua_mount::fs_slot_base(new7, new8, new11, new12) == 9);  // Issue #9
    // Anything else is refused rather than guessed: calling a wrong slot wiped every search path (frozen model load).
    CHECK(lua_mount::fs_slot_base(new7, old8, new11, new12) == -1);  // Issue #9: one trivial, one not
    CHECK(lua_mount::fs_slot_base(new7, new8, new7, new12) == -1);   // Issue #9: AddSearchPath cannot be a bare ret
    CHECK(lua_mount::fs_slot_base(B{}, new8, new11, new12) == -1);   // Issue #9: unreadable code
    // The constant-return shapes MSVC emits for trivial virtuals.
    for (const B &b : {B{0xc3}, B{0xc2, 0x00, 0x00}, B{0x32, 0xc0, 0xc3}, B{0x33, 0xc0, 0xc3}, B{0xb0, 0x01, 0xc3},
                       B{0xb8, 0x04, 0x00, 0x00, 0x00, 0xc3}, B{0x48, 0x33, 0xc0, 0xc3}})
        CHECK(lua_mount::is_constant_return(b));  // Issue #9
    CHECK(!lua_mount::is_constant_return(old7));             // Issue #9
    CHECK(!lua_mount::is_constant_return(B{0xe9, 0, 0, 0, 0}));  // Issue #9: a jump thunk is not followed: not trivial
    CHECK(!lua_mount::is_constant_return(B{0xb8, 0x04, 0x00}));  // Issue #9: truncated
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


// Issue #8: frame rate over a time window, hung / stalled assessment, how long the frame rate stayed low.
static void test_health() {
    using health::fps_over_window;
    auto steady = [](double from, double to, double fps) {
        std::vector<double> v;
        for (double t = from; t <= to + 1e-9; t += 1.0 / fps) v.push_back(t);
        return v;
    };
    // A steady 60 fps for 10 s.
    std::vector<double> a = steady(0, 10, 60);
    double f = fps_over_window(a, 10.0, 3.0);
    CHECK(f > 57 && f < 63);
    // 128 s at 2 fps (a map load), then 3.5 s at 20 fps: the window forgets the slow stretch (the last 256 frames did not).
    std::vector<double> b = steady(0, 128, 2);
    for (double t : steady(128.5, 132, 20)) b.push_back(t);
    f = fps_over_window(b, 132.0, 3.0);
    CHECK(f > 17 && f < 23);
    // Nothing for 10 s (the main thread is blocked): 0, not the rate of before.
    CHECK(fps_over_window(a, 20.0, 3.0) == 0);
    // Blocked for 1.5 s inside the window: reported lower than the nominal rate.
    f = fps_over_window(steady(0, 8.5, 20), 10.0, 3.0);
    CHECK(f > 8 && f < 11.5);
    // Not enough data.
    CHECK(fps_over_window({}, 5.0, 3.0) < 0);
    CHECK(fps_over_window({4.9}, 5.0, 3.0) < 0);
    // Frames at 2000 per second for 5 s (more than the history holds) still read right.
    f = fps_over_window(steady(0, 5, 2000), 5.0, 3.0);
    CHECK(f > 1900 && f < 2100);
    // The jobs frame statistics use the same window: fps and the old fields.
    for (double t : steady(1000, 1128, 2)) jobs::on_frame_at(t);
    for (double t : steady(1128.5, 1132, 20)) jobs::on_frame_at(t);
    json st = jobs::frame_stats_at(1132.0);
    CHECK(st["fps"].is_number() && st["fps"].get<double>() > 17 && st["fps"].get<double>() < 23);
    CHECK(st.contains("frametime_avg_ms") && st.contains("frametime_max_ms") && st.contains("samples"));
    json stalled = jobs::frame_stats_at(1142.0);
    CHECK(stalled["fps"].is_number() && stalled["fps"].get<double>() == 0);

    // assess: a ticking game, a slow one, a hung one, a loading one, a silent one.
    using health::assess;
    auto h = assess(0.02, 1.0, false);
    CHECK(h.stalled_s == 0 && !h.hung);
    h = assess(30.0, 1.0, false);
    CHECK(h.stalled_s > 30 && h.stalled_s < 33 && !h.hung);       // blocked for 30 s: reported, not hung yet
    h = assess(70.0, 1.0, false);
    CHECK(h.hung && h.stalled_s >= 70 && h.hung_after_s == health::kHungAfterS);
    h = assess(70.0, 1.0, true);
    CHECK(!h.hung && h.stalled_s >= 70 && h.hung_after_s == health::kHungAfterLoadingS);  // a map load may take that long
    h = assess(190.0, 1.0, true);
    CHECK(h.hung);
    h = assess(0.02, 8.0, false);                                  // heartbeats stopped: the silence counts
    CHECK(h.stalled_s >= 8 && !h.hung);
    h = assess(0.02, 65.0, false);
    CHECK(h.hung && h.stalled_s >= 65);
    h = assess(0.5, 2.5, false);                                   // an ordinary moment between two heartbeats
    CHECK(h.stalled_s == 0 && !h.hung);
    h = assess(70.0, 1.0, false, 100.0);                           // explicit limit (tests)
    CHECK(!h.hung && h.hung_after_s == 100.0);

    // FpsWatch: the time the rate stayed under 10 fps.
    health::FpsWatch w;
    CHECK(w.low_s(0) == 0);
    for (double t = 0; t <= 10; t += 2) w.feed(t, 3.0, false);
    CHECK(w.low_s(10) >= 10 - 0.01 && w.low_s(10) <= 10.5);
    w.feed(12, 20.0, false);                                       // recovered
    CHECK(w.low_s(12) == 0);
    health::FpsWatch w2;
    for (double t = 0; t <= 4; t += 2) w2.feed(t, 3.0, false);
    CHECK(w2.low_s(4) == 0);                                       // under kLowFpsShownS: not worth a flag yet
    health::FpsWatch w3;
    for (double t = 0; t <= 20; t += 2) w3.feed(t, 1.0, true);     // loading or stalled: those have their own flags
    CHECK(w3.low_s(20) == 0);
    health::FpsWatch w4;
    for (double t = 0; t <= 10; t += 2) w4.feed(t, -1.0, false);   // unknown rate
    CHECK(w4.low_s(10) == 0);
    health::FpsWatch w5;
    w5.feed(0, 3.0, false);
    w5.feed(2, 3.0, false);
    w5.feed(30, 3.0, false);                                       // a gap in the samples starts over
    CHECK(w5.low_s(30) == 0);
}

// Issue #8: the memory check before instances_launch, and the memory facts of a crash report.
static void test_lowmem() {
    using lowmem::Memory;
    // Linux /proc/meminfo.
    Memory m = lowmem::parse_meminfo("MemTotal:       32768000 kB\nMemFree:         1000000 kB\nMemAvailable:    8388608 kB\nSwapFree:        1000 kB\nCommitLimit:    20971520 kB\nCommitted_AS:   10485760 kB\n");
    CHECK(m.known && m.free_ram_mb == 8192 && m.free_commit_mb == 10240);
    CHECK(!lowmem::parse_meminfo("").known);
    CHECK(!lowmem::parse_meminfo("MemTotal: 1 kB\n").known);
    CHECK(!lowmem::parse_meminfo("MemAvailable: 100 kB\nCommitLimit: 100 kB\n").known);  // Committed_AS missing
    CHECK(lowmem::parse_meminfo("MemAvailable: 8388608 kB\nCommitLimit: 100 kB\nCommitted_AS: 5000 kB\n").free_commit_mb == 0);  // over-committed: 0, not a huge unsigned number

    auto mem = [](uint64_t commit, uint64_t ram) { Memory x; x.known = true; x.free_commit_mb = commit; x.free_ram_mb = ram; return x; };
    // Plenty: no refusal.
    auto v = lowmem::check_launch(mem(60000, 20000), 4, 2, false);
    CHECK(!v.refuse);
    // The report of the issue: 11 clients running, free commit 1.8 GB, one more client wanted.
    v = lowmem::check_launch(mem(1780, 4000), 1, 11, false);
    CHECK(v.refuse);
    CHECK(v.details.value("free_commit_mb", 0) == 1780 && v.details.value("running_clients", -1) == 11 && v.details.value("per_client_mb", 0) == (int)lowmem::kPerClientCommitMb);
    CHECK(v.details.value("count", 0) == 1 && v.details.value("needed_commit_mb", 0) > 1780 && v.details.contains("free_ram_mb"));
    CHECK(v.message.find("ignore_low_memory") != std::string::npos && v.message.find("11") != std::string::npos && v.message.find("1780") != std::string::npos);
    // The need grows with the number of clients asked for: enough for one, not for four.
    uint64_t one = lowmem::kPerClientCommitMb + lowmem::kReserveCommitMb;
    CHECK(!lowmem::check_launch(mem(one + 10, 20000), 1, 0, false).refuse);
    CHECK(lowmem::check_launch(mem(one + 10, 20000), 4, 0, false).refuse);
    CHECK(lowmem::check_launch(mem(one - 10, 20000), 1, 0, false).refuse);
    // Free RAM counts too (a machine that swaps drops clients to 2-4 fps).
    CHECK(lowmem::check_launch(mem(60000, 300), 1, 3, false).refuse);
    CHECK(!lowmem::check_launch(mem(60000, lowmem::kPerClientRamMb * 4), 4, 0, false).refuse);
    // The caller's override, and an unknown memory, never refuse.
    CHECK(!lowmem::check_launch(mem(1780, 100), 1, 11, true).refuse);
    CHECK(!lowmem::check_launch(Memory{}, 8, 11, false).refuse);

    // Crash report facts.
    json d = json::object();
    std::string msg = "client c crashed.";
    lowmem::annotate_crash(mem(1800, 900), d, msg);
    CHECK(d["memory"].value("free_commit_mb", 0) == 1800 && d["memory"].value("free_ram_mb", 0) == 900);
    CHECK(msg.find("memory") != std::string::npos && msg.find("1800") != std::string::npos && msg.rfind("client c crashed.", 0) == 0);
    d = json::object();
    msg = "client c crashed.";
    lowmem::annotate_crash(mem(40000, 20000), d, msg);
    CHECK(d["memory"].value("free_commit_mb", 0) == 40000 && msg == "client c crashed.");  // plenty of memory: the figures, no hint
    d = json::object();
    msg = "x";
    lowmem::annotate_crash(Memory{}, d, msg);
    CHECK(!d.contains("memory") && msg == "x");
    CHECK(lowmem::memory_json(Memory{}).is_null() && lowmem::memory_json(mem(5, 6)).value("free_ram_mb", 0) == 6);
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
    tools::register_instances();
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
    auto heartbeat = [&](const std::string &id, const std::string &mode, DWORD pid, bool closing = false, const json &extra = json::object(), int member_port = 1) {
        httplib::Client cli("127.0.0.1", port);
        cli.set_bearer_token_auth(cfg.token);
        json j{{"id", id}, {"port", member_port}, {"pid", pid}, {"mode", mode}, {"game_dir", (game / "garrysmod").u8string()}, {"map", ""}};
        for (auto it = extra.begin(); it != extra.end(); ++it) j[it.key()] = it.value();
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
    // 10. Issue #6: right after this instance took the base port over from a coordinator that went away, an instance it does
    // not know yet may just not have re-registered: the answer is the retryable coordinator_changing, nothing was sent to it.
    {
        CHECK(!coordinator::in_handover() && coordinator::handover_info().is_null());
        coordinator::note_takeover_for_tests(true);
        json e = status_of("late-1");
        CHECK(e.value("code", "") == "coordinator_changing");
        CHECK(e.value("details", json::object()).value("retry_after_s", 0.0) > 0 && e.value("details", json::object()).value("instance", "") == "late-1");
        CHECK(coordinator::in_handover() && coordinator::handover_info().value("state", "") == "changing");
        json sc = mcp::call_tool("instances_list", json::object()).value("structuredContent", json::object());
        CHECK(sc.value("coordinator_changing", false) && sc.value("coordinator_id", "") == "coordinator-test" && sc.value("coordinator", false));
        // A member that registered meanwhile is relayed to as usual (here: its process is alive, its port answers nothing).
        {
            Child c;
            CHECK(c.start() && heartbeat("late-2", "client", c.pi.dwProcessId));
            CHECK(status_of("late-2").value("code", "") == "instance_unreachable");
            c.end(1);
        }
        coordinator::note_takeover_for_tests(false);
        CHECK(status_of("late-1").value("code", "") == "unknown_instance");
        sc = mcp::call_tool("instances_list", json::object()).value("structuredContent", json::object());
        CHECK(!sc.contains("coordinator_changing") && sc.value("coordinator_id", "") == "coordinator-test");
    }
    // 11. Issue #6: game_quit / game_terminate on the coordinator while other instances live say the role moves.
    {
        // The earlier members are dead and purged: nobody else lives, nothing to warn about.
        CHECK(coordinator::leaving_note().is_null());
        Child c;
        CHECK(c.start() && heartbeat("lead-1", "client", c.pi.dwProcessId));
        json n = coordinator::leaving_note();
        CHECK(n.is_object() && n.value("coordinator_moved", false) && n.value("remaining_instances", json::array()) == json::array({"lead-1"}));
        CHECK(n.is_object() && n.value("warning", "").find("coordinator_changing") != std::string::npos);
        c.end(1);
        CHECK(coordinator::leaving_note().is_null());  // it ended: purged, no one left to elect
    }
    for (auto &f : fs::directory_iterator(game / "crashes", ec)) fs::remove(f.path(), ec);  // no dump of the steps above can be matched to the next ones
    // 12. Issue #8: a main thread blocked for a long time. instances_list says how long (stalled_s) and, past the limit, the
    // state is "hung" with advice; while a map load is in progress the limit is longer.
    {
        auto entry_of = [&](const std::string &id) {
            for (const auto &x : coordinator::instances())
                if (x.value("id", "") == id) return x;
            return json();
        };
        Child c;
        CHECK(c.start());
        CHECK(heartbeat("hang-1", "client", c.pi.dwProcessId, false, json{{"last_tick_age_s", 0.01}, {"main_thread_stalled", false}, {"fps", 20}, {"loading", false}}));
        json e = entry_of("hang-1");
        CHECK(e.value("state", "") == "running" && e.value("stalled_s", -1.0) == 0 && e.value("fps_low_s", -1.0) == 0);
        CHECK(heartbeat("hang-1", "client", c.pi.dwProcessId, false, json{{"last_tick_age_s", 30.0}, {"main_thread_stalled", true}, {"fps", 0}, {"loading", false}}));
        e = entry_of("hang-1");
        CHECK(e.value("state", "") == "running" && e.value("stalled_s", 0.0) >= 30 && e.value("stalled_s", 0.0) < 35);
        CHECK(heartbeat("hang-1", "client", c.pi.dwProcessId, false, json{{"last_tick_age_s", 90.0}, {"main_thread_stalled", true}, {"fps", 0}, {"loading", false}}));
        e = entry_of("hang-1");
        CHECK(e.value("state", "") == "hung" && e.value("stalled_s", 0.0) >= 90);
        std::string note = e.value("note", "");
        CHECK(note.find("instances_close") != std::string::npos && note.find("game_terminate") != std::string::npos && note.find("relaunch") != std::string::npos);
        CHECK(e.value("pid", 0u) == c.pi.dwProcessId && e.value("heartbeat_age_s", 99.0) < 3);
        // A map load in progress: 90 s is not hung yet, 200 s is.
        CHECK(heartbeat("hang-1", "client", c.pi.dwProcessId, false, json{{"last_tick_age_s", 90.0}, {"main_thread_stalled", true}, {"fps", 0}, {"loading", true}}));
        e = entry_of("hang-1");
        CHECK(e.value("state", "") == "running" && e.value("stalled_s", 0.0) >= 90 && e["loading"] == true);
        CHECK(heartbeat("hang-1", "client", c.pi.dwProcessId, false, json{{"last_tick_age_s", 200.0}, {"main_thread_stalled", true}, {"fps", 0}, {"loading", true}}));
        CHECK(entry_of("hang-1").value("state", "") == "hung");
        // It recovers: running again.
        CHECK(heartbeat("hang-1", "client", c.pi.dwProcessId, false, json{{"last_tick_age_s", 0.02}, {"main_thread_stalled", false}, {"fps", 20}, {"loading", false}}));
        e = entry_of("hang-1");
        CHECK(e.value("state", "") == "running" && e.value("stalled_s", -1.0) == 0 && !e.contains("note"));
        c.end(1);
    }
    // 13. Issue #8: no heartbeat at all while the process lives (frozen whole, crash dialog): the instance used to vanish from
    // the list after 10 s. It stays listed: "unresponsive", then "hung" past the limit; a clean quit is not listed.
    {
        coordinator::set_timing_for_tests(1.0, 3.0, 0, 0);
        auto entry_of = [&](const std::string &id) {
            for (const auto &x : coordinator::instances())
                if (x.value("id", "") == id) return x;
            return json();
        };
        Child c;
        CHECK(c.start());
        CHECK(heartbeat("silent-1", "client", c.pi.dwProcessId, false, json{{"last_tick_age_s", 0.01}, {"loading", false}}));
        plat::sleep_ms(1600);
        json e = entry_of("silent-1");
        CHECK(e.value("state", "") == "unresponsive" && e.value("stalled_s", 0.0) >= 1.5 && e.value("note", "").find("heartbeat") != std::string::npos);
        plat::sleep_ms(2500);
        e = entry_of("silent-1");
        CHECK(e.value("state", "") == "hung" && e.value("note", "").find("instances_close") != std::string::npos && e.value("pid", 0u) == c.pi.dwProcessId);
        CHECK(heartbeat("silent-1", "client", c.pi.dwProcessId, false, json{{"last_tick_age_s", 0.01}, {"loading", false}}));  // it came back
        CHECK(entry_of("silent-1").value("state", "") == "running");
        plat::sleep_ms(1600);
        CHECK(entry_of("silent-1").value("state", "") == "unresponsive");
        CHECK(heartbeat("silent-1", "client", c.pi.dwProcessId, true));  // it said goodbye: closing, not hung
        plat::sleep_ms(300);
        c.end(0);
        CHECK(entry_of("silent-1").is_null());
        coordinator::set_timing_for_tests(-1, -1, -1, -1);
    }
    // 14. Issue #8: the frame rate stayed low for a while: fps_low_s; not while the game loads.
    {
        coordinator::set_timing_for_tests(0, 0, 0, 1.0);
        auto entry_of = [&](const std::string &id) {
            for (const auto &x : coordinator::instances())
                if (x.value("id", "") == id) return x;
            return json();
        };
        Child c;
        CHECK(c.start());
        for (int i = 0; i < 3; ++i) {
            CHECK(heartbeat("slow-1", "client", c.pi.dwProcessId, false, json{{"last_tick_age_s", 0.2}, {"fps", 3}, {"loading", false}}));
            plat::sleep_ms(700);
        }
        json e = entry_of("slow-1");
        CHECK(e.value("state", "") == "running" && e.value("fps_low_s", 0.0) >= 1.0 && e.value("fps_low_s", 0.0) < 5);
        CHECK(heartbeat("slow-1", "client", c.pi.dwProcessId, false, json{{"last_tick_age_s", 0.01}, {"fps", 20}, {"loading", false}}));
        CHECK(entry_of("slow-1").value("fps_low_s", -1.0) == 0);
        for (int i = 0; i < 3; ++i) {
            CHECK(heartbeat("slow-1", "client", c.pi.dwProcessId, false, json{{"last_tick_age_s", 0.2}, {"fps", 1}, {"loading", true}}));
            plat::sleep_ms(700);
        }
        CHECK(entry_of("slow-1").value("fps_low_s", -1.0) == 0);
        c.end(1);
        coordinator::set_timing_for_tests(-1, -1, -1, -1);
    }
    // 15. Issue #8: the process dies while a call to it is in flight and its connection stays open (a crash dialog, a handle
    // inherited by another process): the call fails at once with client_crashed, it does not wait for its own timeout.
    {
        (void)coordinator::instances();  // ends the earlier children's business now: a dump written later must not be matched to them
        for (auto &f : fs::directory_iterator(game / "crashes", ec)) fs::remove(f.path(), ec);  // nor the dumps of earlier steps to this one
        httplib::Server fake;
        std::atomic<bool> release{false};
        fake.Post("/mcp", [&](const httplib::Request &, httplib::Response &res) {
            for (int i = 0; i < 80 && !release; ++i) plat::sleep_ms(100);  // 8 s
            res.set_content("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"structuredContent\":{\"late\":true}}}", "application/json");
        });
        int fport = fake.bind_to_any_port("127.0.0.1");
        std::thread srv([&] { fake.listen_after_bind(); });
        Child c;
        CHECK(fport > 0 && c.start());
        CHECK(heartbeat("flight-2", "client", c.pi.dwProcessId, false, json::object(), fport));
        write_dump("gmod_win64_20261114_101313_1_accessviolation.mdmp");
        std::thread killer([&] {
            plat::sleep_ms(500);
            c.end(0xC0000005u);
        });
        auto t0 = std::chrono::steady_clock::now();
        json e = status_of("flight-2");
        double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        killer.join();
        CHECK(e.value("code", "") == "client_crashed" && e.value("message", "").find("gmod_win64_20261114_101313_1_accessviolation.mdmp") != std::string::npos);
        CHECK(took < 4.0);
        release = true;
        fake.stop();
        srv.join();
    }
    // 16. Issue #8: the process lives but stopped sending heartbeats while a call is in flight (frozen whole): the call fails
    // with instance_hung once the silence outlasts the limit, naming how to end it.
    {
        coordinator::set_timing_for_tests(1.0, 0, 1.5, 0);
        httplib::Server fake;
        std::atomic<bool> release{false};
        fake.Post("/mcp", [&](const httplib::Request &, httplib::Response &res) {
            for (int i = 0; i < 80 && !release; ++i) plat::sleep_ms(100);
            res.set_content("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"structuredContent\":{\"late\":true}}}", "application/json");
        });
        int fport = fake.bind_to_any_port("127.0.0.1");
        std::thread srv([&] { fake.listen_after_bind(); });
        Child c;
        CHECK(fport > 0 && c.start());
        CHECK(heartbeat("flight-3", "client", c.pi.dwProcessId, false, json::object(), fport));
        auto t0 = std::chrono::steady_clock::now();
        json e = status_of("flight-3");
        double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(e.value("code", "") == "instance_hung");
        CHECK(e.value("message", "").find("game_terminate") != std::string::npos && e.value("message", "").find("instances_close") != std::string::npos);
        CHECK(e.value("details", json::object()).value("pid", 0u) == c.pi.dwProcessId && e["details"].value("silent_s", 0.0) >= 1.5);
        CHECK(took >= 1.4 && took < 5.0);
        // A healthy client whose heartbeats keep coming is not cut off: the call returns the member's own answer.
        CHECK(heartbeat("flight-4", "client", c.pi.dwProcessId, false, json::object(), fport));
        std::atomic<bool> beat{true};
        std::thread beater([&] {
            while (beat) {
                heartbeat("flight-4", "client", c.pi.dwProcessId, false, json::object(), fport);
                plat::sleep_ms(300);
            }
        });
        std::thread releaser([&] {
            plat::sleep_ms(2500);
            release = true;
        });
        json ok = status_of("flight-4");
        beat = false;
        beater.join();
        releaser.join();
        CHECK(ok.is_null() || ok.value("code", "") != "instance_hung");  // no error object: the call went through
        c.end(1);
        fake.stop();
        srv.join();
        coordinator::set_timing_for_tests(-1, -1, -1, -1);
    }

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

// Issue #3: a number the game cannot represent (NaN, infinity, an overflowing coordinate) is written as JSON null, and the
// MCP SDK in Claude Code fails a success result whose field is null where the schema says `number`. Policy: every number
// of every outputSchema accepts null, so no field can fail this way. This walks every declared schema and rejects a bare
// "number" or "integer" type anywhere (properties, items, nested objects, next_call).
static void walk_bare_numbers(const json &node, const std::string &where, int &bad) {
    if (node.is_object()) {
        auto t = node.find("type");
        if (t != node.end()) {
            bool null_ok = false, num = false;
            auto look = [&](const json &x) {
                if (x.is_string() && (x == "number" || x == "integer")) num = true;
                if (x.is_string() && x == "null") null_ok = true;
            };
            if (t->is_array()) for (const auto &x : *t) look(x); else look(*t);
            if (num && !null_ok) {
                std::printf("bare number type at %s\n", where.c_str());
                bad++;
            }
        }
        for (auto it = node.begin(); it != node.end(); ++it) walk_bare_numbers(it.value(), where + "/" + it.key(), bad);
    } else if (node.is_array()) {
        for (size_t i = 0; i < node.size(); i++) walk_bare_numbers(node[i], where + "/" + std::to_string(i), bad);
    }
}
static void test_output_schemas_nonfinite() {
    if (mcp::tools().empty()) tools::register_all();
    int bad = 0;
    for (const auto &t : mcp::tools()) walk_bare_numbers(t.output_schema, t.name, bad);
    CHECK(bad == 0);
    // The guide tells the reader that a number the game cannot hold reads null.
    json g = resources::read("mcpagent://guide");
    std::string gt = g.is_object() && g.contains("text") ? g["text"].get<std::string>() : "";
    CHECK(gt.find("non-finite") != std::string::npos && gt.find("null") != std::string::npos);
    // The four live mismatches of the chaos fuzz: huge coordinates, an overflowing eye angle, a NaN origin, "1e999".
    auto matches = [&](const char *tool, const json &r) {
        const mcp::Tool *t = mcp::find_tool(tool);
        if (!t) return false;
        nlohmann::json_schema::json_validator v;
        v.set_root_schema(t->output_schema);
        try { v.validate(r); } catch (const std::exception &) { return false; }
        return true;
    };
    json nul = nullptr;
    CHECK(matches("trace", json{{"hit", false}, {"pos", json::array({nul, nul, nul})}, {"fraction", 1}, {"distance", nul}}));
    CHECK(matches("trace_through", json{{"surfaces", json::array()}, {"count", 0}, {"total_thickness", 0}, {"distance", nul}, {"truncated", false}}));
    CHECK(matches("player_set", json{{"id", 1}, {"eye_ang", json::array({nul, 0, 0})}}));
    CHECK(matches("players_list", json{{"players", json::array({json{{"eye_ang", json::array({nul, 0, 0})}}})}, {"count", 1}}));
    CHECK(matches("ents_find", json{{"entities", json::array({json{{"id", -1}, {"pos", json::array({nul, nul, nul})}, {"ang", json::array({nul, 0, 0})}}})}, {"count", 1}, {"truncated", false}}));
    CHECK(matches("convar_get", json{{"name", "x"}, {"flags", 0}, {"flag_names", json::array()}, {"number", nul}}));
    CHECK(matches("convar_set", json{{"name", "x"}, {"flags", 0}, {"flag_names", json::array()}, {"number", nul}}));
    // Still strict about the type: a string is not a number.
    CHECK(!matches("trace", json{{"hit", false}, {"pos", json::array({"1", 2, 3})}, {"fraction", 1}, {"distance", 1}}));
}

// Issue #3 (live fuzz): wiki_page answered `unsupported "HTTP function unavailable in this realm"` for every uncached page
// when the menu realm of the game build has no HTTP function. The GET is tried in the menu realm first, then in the client
// and server realms (same hard-coded wiki allowlist in the Lua side); on srcds only the server realm exists.
static void test_wiki_http_realms() {
    if (mcp::tools().empty()) tools::register_all();
    auto cm = tools::wiki_http_realms(true);
    CHECK(cm.size() == 3 && cm[0] == realms::MENU && cm[1] == realms::CLIENT && cm[2] == realms::SERVER);
    auto sv = tools::wiki_http_realms(false);
    CHECK(sv.size() == 1 && sv[0] == realms::SERVER);
    // debug_draw accepts the "arrow" primitive (issue #3 live fuzz: it was rejected).
    const mcp::Tool *dd = mcp::find_tool("debug_draw");
    CHECK(dd != nullptr);
    if (dd) {
        json e = dd->input_schema["properties"]["primitives"]["items"]["properties"]["type"]["enum"];
        bool arrow = false;
        for (const auto &x : e) arrow = arrow || x == "arrow";
        CHECK(arrow);
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

    // Issue #11: lua_job {wait: false} used to be answered "did you mean 'jit'?" (the closest spelling). A wait-like key means
    // `async`; since `wait` and `async` are opposites, the corrected call inverts a boolean.
    auto err_of = [](const json &res) { return res["structuredContent"]["error"]; };
    json w = mcp::call_tool("lua_job", json{{"code", "return 1"}, {"wait", false}});
    CHECK(w.value("isError", false) && err_of(w).value("message", "").find("did you mean 'async'") != std::string::npos);
    CHECK(err_of(w).value("message", "").find("did you mean 'jit'") == std::string::npos);
    CHECK(err_of(w).value("next_call", json()) == (json{{"tool", "lua_job"}, {"arguments", {{"code", "return 1"}, {"async", true}}}}));
    json w2 = mcp::call_tool("lua_job", json{{"code", "return 1"}, {"wait", true}});
    CHECK(err_of(w2).value("next_call", json()) == (json{{"tool", "lua_job"}, {"arguments", {{"code", "return 1"}, {"async", false}}}}));
    json bg = mcp::call_tool("lua_job", json{{"code", "return 1"}, {"background", true}});
    CHECK(err_of(bg).value("message", "").find("did you mean 'async'") != std::string::npos);
    CHECK(err_of(bg).value("next_call", json()) == (json{{"tool", "lua_job"}, {"arguments", {{"code", "return 1"}, {"async", true}}}}));
    // The message says what the suggestion means; a wait given as a number cannot be inverted: suggestion without a corrected call.
    CHECK(err_of(w).value("message", "").find("opposite") != std::string::npos);
    json wn = mcp::call_tool("lua_job", json{{"code", "return 1"}, {"wait", 5}});
    CHECK(err_of(wn).value("message", "").find("did you mean 'async'") != std::string::npos && err_of(wn).value("next_call", json()).is_null());
    // `async` already given: no corrected call (it would collide). A tool without `async` keeps the generic suggestion.
    json both = mcp::call_tool("lua_job", json{{"code", "return 1"}, {"async", true}, {"wait", false}});
    CHECK(err_of(both).value("next_call", json()).is_null());
    json other = mcp::call_tool("console_exec", json{{"cmd", "echo hi"}, {"wait", false}});
    CHECK(err_of(other).value("message", "").find("'async'") == std::string::npos);
}

// Issue #5: a foreign program on the base port must not be reported as a token mismatch. A gmod-mcp coordinator that
// refuses the token (401 from gmod-mcp) is "refused"; any other answer (404, 403, a 401 of another program) is "foreign".
static void test_port_conflict_link() {
    // Pure: what counts as a gmod-mcp answer.
    CHECK(gmcp::is_gmod_mcp_reply(true, ""));  // the signature header alone is enough
    CHECK(gmcp::is_gmod_mcp_reply(false, R"({"error":{"code":"unauthorized","message":"missing or invalid bearer token"}})"));  // 0.3.6
    CHECK(gmcp::is_gmod_mcp_reply(false, R"({"jsonrpc":"2.0","id":1,"result":{}})"));
    CHECK(!gmcp::is_gmod_mcp_reply(false, ""));
    CHECK(!gmcp::is_gmod_mcp_reply(false, "<html><body>404 Not Found</body></html>"));
    CHECK(!gmcp::is_gmod_mcp_reply(false, R"({"error":"unauthorized"})"));
    CHECK(!gmcp::is_gmod_mcp_reply(false, R"({"error":{"code":401}})"));

    // The module's own server signs every answer, the rejections included.
    auto &cfg = agent::config();
    std::string saved_token = cfg.token, saved_id = cfg.instance_id;
    int saved_base = cfg.base_port, saved_port = cfg.port;
    cfg.token = "coretests-token-0123456789abcdef";
    cfg.instance_id = "port-conflict-test";
    int real = httpsrv::start(39700, 50, cfg.token);
    CHECK(real != 0);
    if (!real) return;
    {
        httplib::Client cli("127.0.0.1", real);
        auto r = cli.Post("/mcp", "{}", "application/json");  // no token: 401
        CHECK(r && r->status == 401 && r->has_header(gmcp::kSignatureHeader));
        cli.set_bearer_token_auth(cfg.token);
        auto ok = cli.Post("/mcp", R"({"jsonrpc":"2.0","id":1,"method":"ping"})", "application/json");
        CHECK(ok && ok->status == 200 && ok->has_header(gmcp::kSignatureHeader));
    }

    // A program that is not gmod-mcp holds the base port; the member's heartbeat gets its answers.
    struct Case { int status; const char *body; const char *header; const char *expect; };
    const Case cases[] = {
        {404, "<html>not found</html>", nullptr, "foreign"},                                  // the case of the issue
        {403, "", nullptr, "foreign"},
        {401, R"({"error":"unauthorized"})", nullptr, "foreign"},                           // another program's 401
        {401, R"({"error":{"code":"unauthorized","message":"x"}})", nullptr, "refused"},    // gmod-mcp 0.3.6 coordinator
        {401, "", "X-Gmod-Mcp", "refused"},                                                  // gmod-mcp coordinator that signs its answers
    };
    for (const Case &c : cases) {
        httplib::Server foreign;
        foreign.Post(R"(.*)", [&](const httplib::Request &, httplib::Response &res) {
            res.status = c.status;
            if (c.header) res.set_header(c.header, "0.3.6");
            if (*c.body) res.set_content(c.body, "application/json");
        });
        int fport = foreign.bind_to_any_port("127.0.0.1");
        std::thread t([&] { foreign.listen_after_bind(); });
        cfg.base_port = fport;
        cfg.port = real;
        coordinator::start();  // registers with the "coordinator" on fport: one synchronous heartbeat
        CHECK(!coordinator::is_coordinator());
        std::string got = coordinator::link_state();
        if (got != c.expect) std::printf("  HTTP %d %s -> link_state '%s', expected '%s'\n", c.status, c.body, got.c_str(), c.expect);
        CHECK(got == c.expect);
        coordinator::stop();
        foreign.stop();
        t.join();
    }
    httpsrv::stop();
    cfg.token = saved_token;
    cfg.instance_id = saved_id;
    cfg.base_port = saved_base;
    cfg.port = saved_port;
}

// Issue #9: the mounted folder's provenance (which copy of a file a realm really loaded), what changed on disk since
// each realm loaded it, and reloading it on demand. No game: the load records are fed through the same functions the
// MountLua hook and the runtime injection call.
static void set_mtime(const fs::path &p, int seconds_from_now) {
    fs::last_write_time(p, fs::file_time_type::clock::now() + std::chrono::seconds(seconds_from_now));
}
static bool has_str(const json &arr, const std::string &s) {
    for (const auto &e : arr)
        if (e.is_string() && e.get<std::string>() == s) return true;
    return false;
}
static const json *find_file(const json &state, const std::string &rel) {
    for (const auto &f : state["stale_files"])
        if (f.value("file", "") == rel) return &f;
    return nullptr;
}

static void test_issue9_mount_reload() {
    fs::path base = fs::temp_directory_path() / "gmod-mcp-coretests" / "i9";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::path game = base / "garrysmod", ov = base / "wt";
    touch(game / "lua/pba/x.lua");          // the installed copy of a file that the mounted folder shadows
    touch(game / "addons/pba/lua/pba/core.lua");
    touch(ov / "lua/pba/x.lua");
    touch(ov / "lua/pba/sv_relay.lua");
    touch(ov / "lua/pba/cl_hud.lua");
    touch(ov / "lua/pba/sh_shared.lua");
    touch(ov / "lua/entities/pba_ent/init.lua");
    touch(ov / "gamemodes/ovgm/gamemode/init.lua");
    std::ofstream(ov / "lua/readme.txt") << "not lua";
    auto &cfg = agent::config();
    cfg.game_dir = game.u8string();
    cfg.tmp_dir = (game / "lua/mcpagent_tmp").u8string();
    const int SV = realms::SERVER, CL = realms::CLIENT;
    lua_mount::forget_loads_for_tests();
    lua_mount::set_current_for_tests(ov.u8string());

    // scan: every .lua under lua/ and gamemodes/, named from the mount root (the engine's names), nothing else.
    auto files = lua_mount::scan_files(ov.u8string());
    CHECK(files.size() == 6);  // Issue #9
    CHECK(files.count("lua/pba/x.lua") == 1 && files.count("gamemodes/ovgm/gamemode/init.lua") == 1);  // Issue #9
    CHECK(files.count("lua/readme.txt") == 0);  // Issue #9: not Lua
    CHECK(files["lua/pba/x.lua"].mtime == lua_mount::mtime_of((ov / "lua/pba/x.lua").u8string()));  // Issue #9

    // No record yet: nothing is known to come from the mount, and nothing is reported as changed.
    CHECK(!lua_mount::realm_loaded_with_mount(SV));  // Issue #9
    CHECK(lua_mount::changed_files(SV).empty());  // Issue #9
    CHECK(tools::mount_state({SV, CL})["realms_without_mount"].size() == 2);  // Issue #9: both realms predate the mount

    // The server loaded with the mounted folder in front (hook, then injection); the client did not (no hook before it).
    lua_mount::note_load_start(SV, ov.u8string(), files);
    CHECK(!lua_mount::realm_loaded_with_mount(SV));  // Issue #9: only once the realm's runtime was injected: a pending load
    lua_mount::note_realm_loaded(SV);
    lua_mount::note_realm_loaded(CL);  // no load start recorded for the client: it loaded without the mount
    CHECK(lua_mount::realm_loaded_with_mount(SV) && !lua_mount::realm_loaded_with_mount(CL));  // Issue #9

    // Provenance: server copy comes from the mount, the client's (same chunk name) from the installed game.
    json p = tools::source_provenance(SV, "@lua/pba/x.lua");
    CHECK(p["from_mount"] == true && p["mount_has_file"] == true && same(p["disk"], ov / "lua/pba/x.lua"));  // Issue #9
    CHECK(p["provenance"] == "mount_in_place_at_load");  // Issue #9: says how it knows
    p = tools::source_provenance(CL, "@lua/pba/x.lua");
    CHECK(p["from_mount"] == false && p["mount_has_file"] == true && same(p["disk"], game / "lua/pba/x.lua"));  // Issue #9: the installed copy
    CHECK(p["provenance"] == "installed");  // Issue #9
    p = tools::source_provenance(CL, "@lua/pba/sv_relay.lua");  // only in the mount, which this realm never saw: no copy on disk it ran
    CHECK(p["from_mount"] == false && p["mount_has_file"] == true && !p.contains("disk"));  // Issue #9
    p = tools::source_provenance(SV, "@addons/pba/lua/pba/core.lua");
    CHECK(p["from_mount"] == false && p["mount_has_file"] == false && same(p["disk"], game / "addons/pba/lua/pba/core.lua"));  // Issue #9
    p = tools::source_provenance(SV, "=mcpagent:inspect");
    CHECK(p["from_mount"] == false && p["provenance"] == "no_file" && !p.contains("disk"));  // Issue #9: code without a file
    p = tools::source_provenance(SV, "@gamemodes/ovgm/gamemode/init.lua");
    CHECK(p["from_mount"] == true && same(p["disk"], ov / "gamemodes/ovgm/gamemode/init.lua"));  // Issue #9: gamemode files too
    // A file added to the mounted folder after the load was not loaded from it.
    touch(ov / "lua/pba/late.lua");
    p = tools::source_provenance(SV, "@lua/pba/late.lua");
    CHECK(p["from_mount"] == false && p["mount_has_file"] == true);  // Issue #9: not in the load-time snapshot

    CHECK(lua_mount::changed_files(CL).empty());  // Issue #9: no record for the client: nothing to compare with
    json st0 = tools::mount_state({SV, CL});
    CHECK(has_str(st0["realms_without_mount"], "client") && !has_str(st0["realms_without_mount"], "server"));  // Issue #9
    CHECK(st0["loaded_with_mount"]["server"] == true && st0["loaded_with_mount"]["client"] == false);  // Issue #9
    CHECK(tools::mount_warning(st0).find("map_load") != std::string::npos);  // Issue #9: the way out for a realm that predates the mount
    // Running a file of the folder again in a realm that loaded without it: from then on that file comes from the folder.
    lua_mount::note_reloaded(CL, ov.u8string(), "lua/pba/x.lua", lua_mount::mtime_of((ov / "lua/pba/x.lua").u8string()));
    p = tools::source_provenance(CL, "@lua/pba/x.lua");
    CHECK(p["from_mount"] == true && p["provenance"] == "reloaded_from_mount" && same(p["disk"], ov / "lua/pba/x.lua"));  // Issue #9
    CHECK(!lua_mount::realm_loaded_with_mount(CL));  // Issue #9: one reloaded file does not make the realm "loaded with the mount"
    // A new map: the client now loads with the folder in front (a load start after its injection counts for it).
    lua_mount::note_load_start(CL, ov.u8string(), files);
    CHECK(lua_mount::realm_loaded_with_mount(CL));  // Issue #9
    CHECK(tools::mount_state({SV, CL})["realms_without_mount"].empty());  // Issue #9

    // Changes since each realm loaded: modified (older or newer), added, deleted. The unchanged file is not listed.
    set_mtime(ov / "lua/pba/sv_relay.lua", 30);
    set_mtime(ov / "lua/pba/cl_hud.lua", -3600);  // a git checkout can bring an OLDER time: still a change
    fs::remove(ov / "lua/pba/sh_shared.lua");
    auto ch = lua_mount::changed_files(SV);
    auto reason = [&](const std::string &rel) {
        for (const auto &c : ch)
            if (c.rel == rel) return c.reason;
        return std::string();
    };
    CHECK(reason("lua/pba/sv_relay.lua") == "modified" && reason("lua/pba/cl_hud.lua") == "modified");  // Issue #9
    CHECK(reason("lua/pba/sh_shared.lua") == "deleted" && reason("lua/pba/late.lua") == "added");  // Issue #9
    CHECK(reason("lua/pba/x.lua").empty() && reason("lua/readme.txt").empty());  // Issue #9

    // Reload: running the file again records the time it had, so it is no longer reported.
    lua_mount::note_reloaded(SV, ov.u8string(), "lua/pba/sv_relay.lua", lua_mount::mtime_of((ov / "lua/pba/sv_relay.lua").u8string()));
    ch = lua_mount::changed_files(SV);
    CHECK(reason("lua/pba/sv_relay.lua").empty() && reason("lua/pba/cl_hud.lua") == "modified");  // Issue #9

    // status.lua_mount: stale files with the realms each runs in (sv_ -> server, cl_ -> client, shared -> both).
    json st = tools::mount_state({SV, CL});
    CHECK(st["path"] == ov.u8string());  // Issue #9
    CHECK(st["stale_files"].is_array() && find_file(st, "lua/pba/cl_hud.lua") && find_file(st, "lua/pba/sh_shared.lua") &&
          find_file(st, "lua/pba/late.lua") && !find_file(st, "lua/pba/sv_relay.lua"));  // Issue #9
    if (find_file(st, "lua/pba/cl_hud.lua")) {
        const json &f = *find_file(st, "lua/pba/cl_hud.lua");
        CHECK(f["reason"] == "modified" && same(f["disk"], ov / "lua/pba/cl_hud.lua") && f["realms"].size() == 1 && has_str(f["realms"], "client"));  // Issue #9: a cl_ file runs on the client only
    }
    CHECK(!find_file(st, "lua/pba/sv_relay.lua") && find_file(st, "lua/pba/sh_shared.lua") && (*find_file(st, "lua/pba/sh_shared.lua"))["realms"].size() == 2);  // Issue #9: shared: both realms
    std::string w = tools::mount_warning(st);
    CHECK(w.find("lua_reload") != std::string::npos && w.find("cl_hud.lua") != std::string::npos);  // Issue #9
    CHECK(st.contains("warning") && st["warning"] == w);  // Issue #9
    CHECK(tools::mount_warning(json{{"stale_files", json::array()}, {"realms_without_mount", json::array()}}).empty());  // Issue #9: nothing stale, no warning

    // Reload plan: what lua_reload {mounted_changed} would run, per realm. Deleted files and scripted classes are left out.
    json plan = tools::plan_mount_reload(st);
    bool has_cl = false, has_deleted = false;
    for (const auto &it : plan["reload"]) {
        if (it["file"] == "lua/pba/cl_hud.lua") has_cl = true;
        if (it["file"] == "lua/pba/sh_shared.lua") has_deleted = true;
    }
    CHECK(has_cl && !has_deleted);  // Issue #9
    CHECK(plan["skipped"].is_array());  // Issue #9
    touch(ov / "lua/entities/pba_ent/shared.lua");  // a scripted class file: its table (ENT) is built by the engine
    set_mtime(ov / "lua/entities/pba_ent/init.lua", 40);
    plan = tools::plan_mount_reload(tools::mount_state({SV, CL}));
    bool ent_reloaded = false, ent_skipped = false;
    for (const auto &it : plan["reload"]) ent_reloaded |= it["file"] == "lua/entities/pba_ent/init.lua";
    for (const auto &it : plan["skipped"]) ent_skipped |= it["file"] == "lua/entities/pba_ent/init.lua" && !it.value("why", "").empty();
    CHECK(!ent_reloaded && ent_skipped);  // Issue #9
    // Alphabetical order, one entry per file.
    std::vector<std::string> order;
    for (const auto &it : plan["reload"]) order.push_back(it["file"]);
    CHECK(std::is_sorted(order.begin(), order.end()));  // Issue #9

    // No mount: no state, no warning.
    lua_mount::set_current_for_tests("");
    CHECK(tools::mount_state({SV, CL}).is_null() && tools::mount_warning(json()).empty());  // Issue #9
    // A mount that changed since the realm loaded (another folder): the realm ran the old folder's code.
    lua_mount::set_current_for_tests((base / "wt2").u8string());
    CHECK(!lua_mount::realm_loaded_with_mount(SV));  // Issue #9
    lua_mount::set_current_for_tests("");

    // lua_reload {mounted_changed}: exactly one of path / mounted_changed; with nothing changed it runs nothing.
    tools::register_core();
    json r = mcp::call_tool("lua_reload", json::object());
    CHECK(r.value("isError", false) && r.dump().find("invalid_params") != std::string::npos);  // Issue #9: neither path nor mounted_changed
    r = mcp::call_tool("lua_reload", json{{"path", "pba/x.lua"}, {"mounted_changed", true}});
    CHECK(r.value("isError", false) && r.dump().find("invalid_params") != std::string::npos);  // Issue #9: both
    lua_mount::set_current_for_tests(ov.u8string());
    r = mcp::call_tool("lua_reload", json{{"mounted_changed", true}});
    CHECK(!r.value("isError", false) && r.dump().find("\"reloaded\"") != std::string::npos);  // Issue #9: nothing stale in a realm that is not there
    lua_mount::set_current_for_tests("");
    lua_mount::forget_loads_for_tests();
    fs::remove_all(base, ec);
}


#ifdef _WIN32
// Issue #6: a real cluster of three processes sharing a base port. Each runs the module's own server and coordinator code
// (httpsrv + coordinator, no game): `gmod-mcp-coretests --cluster-node <base port> <id>`. The coordinator (first one) is
// killed like a game that crashes; the test measures how long the survivors need to answer on the base port again, and
// that a call for one of them is not answered `unknown_instance` meanwhile.
static const char *kClusterToken = "coretests-cluster-token-0123456789abcdef";

static int cluster_node(int base, const std::string &id) {
    auto &cfg = agent::config();
    cfg.token = kClusterToken;
    cfg.instance_id = id;
    cfg.base_port = base;
    cfg.game_dir = (fs::temp_directory_path() / "gmod-mcp-coretests-cluster" / id).u8string();
    tools::register_core();
    tools::register_instances();
    cfg.port = httpsrv::start(base, 16, cfg.token);
    if (!cfg.port) return 2;
    coordinator::start();
    for (int i = 0; i < 1200; ++i) plat::sleep_ms(100);  // 2 minutes at most: a failed test leaves nothing behind
    return 0;
}

struct Node {
    PROCESS_INFORMATION pi{};
    bool alive = false;
    bool start(int base, const std::string &id) {
        char exe[MAX_PATH * 2] = {};
        GetModuleFileNameA(nullptr, exe, sizeof exe);
        std::string cmd = std::string("\"") + exe + "\" --cluster-node " + std::to_string(base) + " " + id;
        STARTUPINFOA si{};
        si.cb = sizeof si;
        alive = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != 0;
        return alive;
    }
    void kill() {
        if (!alive) return;
        TerminateProcess(pi.hProcess, 0xC0000005u);
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        alive = false;
    }
    ~Node() { kill(); }
};

// tools/call on a port; returns the structuredContent, or null when nothing answered.
static json cluster_call(int port, const std::string &tool, const json &args) {
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(1, 0);
    cli.set_read_timeout(5, 0);
    cli.set_bearer_token_auth(kClusterToken);
    json req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", {{"name", tool}, {"arguments", args}}}};
    auto r = cli.Post("/mcp", req.dump(), "application/json");
    if (!r || r->status != 200) return json();
    try {
        json j = json::parse(r->body);
        return j.value("result", json::object()).value("structuredContent", json::object());
    } catch (...) {
        return json();
    }
}

static std::set<std::string> cluster_ids(const json &list) {
    std::set<std::string> ids;
    if (list.is_object())
        for (const auto &e : list.value("instances", json::array())) ids.insert(e.value("id", ""));
    return ids;
}

static void test_coordinator_handover_cluster() {
    int base = 39800 + (int)(GetCurrentProcessId() % 100) * 16;
    Node a, b, c;
    CHECK(a.start(base, "node-a"));
    // Wait for the first node to hold the base port, then start the two members.
    auto until = [](auto pred, double max_s) {
        auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < max_s) {
            if (pred()) return true;
            plat::sleep_ms(50);
        }
        return pred();
    };
    CHECK(until([&] { return cluster_call(base, "instances_list", json::object()).is_object(); }, 15));
    CHECK(b.start(base, "node-b"));
    CHECK(c.start(base, "node-c"));
    std::set<std::string> all{"node-a", "node-b", "node-c"};
    CHECK(until([&] { return cluster_ids(cluster_call(base, "instances_list", json::object())) == all; }, 20));
    json first = cluster_call(base, "instances_list", json::object());
    CHECK(first.value("coordinator_id", "") == "node-a" && first.value("coordinator", false));

    // The coordinator dies.
    auto t0 = std::chrono::steady_clock::now();
    auto since = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    a.kill();
    std::set<std::string> survivors{"node-b", "node-c"};
    double t_answer = -1, t_full = -1;
    // From the first answer on, a call for a survivor must be relayed or answered retryable, never unknown_instance. The
    // target has no game, so the call ends after its 1 s timeout: done in the background not to slow the polling.
    auto probe = [&](const std::string &id) {
        json r = cluster_call(base, "lua_exec", json{{"instance", id}, {"code", "return 1"}, {"realm", "menu"}, {"timeout_s", 1}});
        return r.value("error", json::object()).value("code", "");
    };
    std::future<std::string> probe_b, probe_c;
    while (since() < 25 && t_full < 0) {
        json l = cluster_call(base, "instances_list", json::object());
        if (l.is_object()) {
            if (t_answer < 0) {
                t_answer = since();
                probe_b = std::async(std::launch::async, probe, "node-b");
                probe_c = std::async(std::launch::async, probe, "node-c");
            }
            if (cluster_ids(l).count("node-b") && cluster_ids(l).count("node-c")) t_full = since();
        }
        plat::sleep_ms(50);
    }
    std::string code_b = probe_b.valid() ? probe_b.get() : "", code_c = probe_c.valid() ? probe_c.get() : "";
    bool unknown_seen = code_b == "unknown_instance" || code_c == "unknown_instance";
    std::printf("  hand-over: base port answers again after %.2f s, both survivors listed after %.2f s\n", t_answer, t_full);
    CHECK(t_answer >= 0 && t_full >= 0);
    CHECK(t_full >= 0 && t_full < 5.0);  // was 9 to 15 s: 3 slow heartbeats (a refused loopback connection takes 2 s on Windows), then the others re-registered at their own pace
    CHECK(!unknown_seen);
    json now = cluster_call(base, "instances_list", json::object());
    int coords = 0;
    std::string who;
    for (const auto &e : now.value("instances", json::array()))
        if (e.value("role", "") == "coordinator") {
            ++coords;
            who = e.value("id", "");
        }
    CHECK(coords == 1 && (who == "node-b" || who == "node-c") && now.value("coordinator_id", "") == who);
    CHECK(cluster_ids(now) == survivors);
    // A member relays through the new coordinator to the other survivor.
    std::string other = who == "node-b" ? "node-c" : "node-b";
    json st = cluster_call(base, "lua_exec", json{{"instance", other}, {"code", "return 1"}, {"realm", "menu"}, {"timeout_s", 1}});
    if (st.value("relayed_by", "") != who) std::printf("  relay answer: %s\n", st.dump().c_str());
    CHECK(st.value("relayed_by", "") == who && st.value("error", json::object()).value("code", "") != "unknown_instance");
    b.kill();
    c.kill();
}
#endif

int main(int argc, char **argv) {
#ifdef _WIN32
    if (argc >= 4 && std::string(argv[1]) == "--cluster-node") return cluster_node(std::atoi(argv[2]), argv[3]);
#endif
    (void)argc;
    (void)argv;
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // failures stay visible even if a test crashes the process (CI pipes stdout)
    test_installer_roundtrip();
    test_guide_result_conventions();
    test_output_schemas();
    test_output_schemas_nonfinite();
    test_wiki_http_realms();
    test_next_call();
    test_cmdline();
    test_crash_report();
    test_health();
    test_lowmem();
    test_console_wait();
    test_errors_map_tags();
    test_errors_filter_before_limit();
    test_errors_group_by_message();
    test_fields_note();
    test_long_waits();
    test_errors_load_stamp_order();
    test_small_b6();
    test_srcds_error_parsing();
    test_console_never_throws();
    test_console_read_regex_outside_lock();
    test_edge_cases();
    test_argcheck();
    test_job_ids();
    test_lua_mount_paths();
    test_fs_vtable_layout();
    test_issue9_mount_reload();
    test_game_exe_layouts();
    test_launch_options();
    test_port_conflict_link();
#ifdef _WIN32
    test_crash_report_coordinator();
    test_coordinator_handover_cluster();  // Issue #6
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

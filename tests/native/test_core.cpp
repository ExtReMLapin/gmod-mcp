// Unit tests of the module's tool helpers that need no game: Lua path lookup and the `fields` projection.
// Built as gmod-mcp-coretests (links the module's sources like gmod-mcp-snapshot), run by the CI.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "agent.h"
#include "cmdline.h"
#include "console.h"
#include "coordinator.h"
#include "mcp.h"
#include "overlay.h"
#include "resources.h"
#include "overlay.h"
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
}

// B1 overlay: which folders are accepted as an overlay root (no game needed for this part).
static void test_overlay_paths() {
    fs::path t = fs::temp_directory_path() / "gmod-mcp-coretests-overlay";
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
        bool r = overlay::normalize_root(in.u8string(), root, err);
        return r && same(root, want) && root.back() != '/' && root.back() != '\\';
    };
    auto bad = [&](const std::string &in, const char *word) {
        root.clear();
        err.clear();
        bool r = overlay::normalize_root(in, root, err);
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
    auto args = util::split_args(R"("C:\Games\gmod_win64.exe" -multirun -mcp -mcp_overlay "D:\dev\my addon wt" +fps_max 20)");
    CHECK(util::flag_value(args, "-mcp_overlay") == R"(D:\dev\my addon wt)");
    CHECK(util::flag_value(util::split_args("gmod -mcp"), "-mcp_overlay").empty());
    // The tool refuses a bad path at once (before touching the game's main thread).
    tools::register_overlay();
    json r = mcp::call_tool("lua_overlay", json{{"path", "addons/pba"}});
    CHECK(r.value("isError", false) && r.dump().find("invalid_params") != std::string::npos);
    r = mcp::call_tool("lua_overlay", json{{"path", (t / "nolua").u8string()}});
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
    // Overlay paths.
    fs::path t = fs::temp_directory_path() / "gmod-mcp-edge-overlay";
    std::error_code ec;
    fs::remove_all(t, ec);
    fs::create_directories(t / "\xC3\xA9t\xC3\xA9 wt" / "lua");
    fs::create_directories(t / "garrysmod" / "lua");
    fs::create_directories(t / "garrysmod" / "addons" / "pba" / "lua");
    std::ofstream(t / "afile.txt") << "x";
    std::string root, err;
    CHECK(overlay::normalize_root((t / "\xC3\xA9t\xC3\xA9 wt").u8string(), root, err));
    CHECK(overlay::normalize_root((t / "\xC3\xA9t\xC3\xA9 wt" / "lua").u8string() + "/", root, err) &&
          fs::u8path(root) == (t / "\xC3\xA9t\xC3\xA9 wt").make_preferred());
    CHECK(!overlay::normalize_root((t / "afile.txt").u8string(), root, err) && err.find("not a folder") != std::string::npos);
    CHECK(!overlay::normalize_root("\\\\nosuchserver\\share\\wt", root, err));
    CHECK(!overlay::normalize_root(t.u8string() + "/does-not-exist", root, err) && err.find("not found") != std::string::npos);
    std::string game = (t / "garrysmod").u8string();
    CHECK(!overlay::refuse_inside_game((t / "garrysmod").u8string(), game).empty());
    CHECK(!overlay::refuse_inside_game((t / "garrysmod" / "addons" / "pba").u8string(), game).empty());
    CHECK(!overlay::refuse_inside_game(game + "/", game).empty());
    CHECK(overlay::refuse_inside_game((t / "\xC3\xA9t\xC3\xA9 wt").u8string(), game).empty());
    CHECK(overlay::refuse_inside_game((t / "garrysmod-wt").u8string(), game).empty());  // a sibling with the same prefix
    fs::remove_all(t, ec);
}

int main() {
    test_cmdline();
    test_console_wait();
    test_errors_map_tags();
    test_small_b6();
    test_srcds_error_parsing();
    test_edge_cases();
    test_overlay_paths();
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

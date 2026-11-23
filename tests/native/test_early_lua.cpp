// Issue #20 (docs/TESTS.md EL-1): the pure part of early_lua (src/early_lua.cpp): tokens, the early autorun file, sweeping the
// temporary folders of clients that are gone. No game. (The Lua-level data/ isolation that used to live here was removed.)
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "check.h"
#include "early_lua.h"

namespace fs = std::filesystem;
using namespace earlylua;

static void put(const fs::path &p, const std::string &content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << content;
}

static void test_tokens() {
    CHECK(valid_token("c0123456789a") && valid_token("a_b-C9") && valid_token(std::string(40, 'x')));
    CHECK(!valid_token("") && !valid_token(std::string(41, 'x')) && !valid_token("a b") && !valid_token("../x") && !valid_token("a/b") &&
          !valid_token("a\"b") && !valid_token("a;b") && !valid_token("é"));
    std::string a = new_token(), b = new_token();
    CHECK(valid_token(a) && a.size() == 12 && a[0] == 'c' && a != b);
    CHECK(check_early_lua("print(1)").empty() && check_early_lua("").empty());
    CHECK(!check_early_lua(std::string(kMaxEarlyLua + 1, 'x')).empty() && check_early_lua(std::string(kMaxEarlyLua, 'x')).empty());
    CHECK(!check_early_lua(std::string("a\0b", 3)).empty());
}

static void test_shim() {
    // early_lua runs in its own protected chunk; nothing else is in the file (no file library is touched).
    std::string e = build_shim("RunConsoleCommand('x_enable', '0')");
    CHECK(e.find("RunConsoleCommand('x_enable', '0')") != std::string::npos);
    CHECK(e.find("CompileString(src, \"mcpagent_early_lua\", false)") != std::string::npos && e.find("pcall(f)") != std::string::npos);
    CHECK(e.find("file.") == std::string::npos && e.find("overlay") == std::string::npos && e.find("__mcpagent_data") == std::string::npos);
    // No early_lua: no compile step (the module writes no file then; this is only the defined result).
    CHECK(build_shim("").find("CompileString") == std::string::npos);
    // Lua long brackets inside the user's code do not end the string early.
    std::string nasty = "local t = [[a]] local u = [=[b]=] return ]==]";
    std::string n = build_shim(nasty);
    CHECK(n.find("local src = [===[") != std::string::npos && n.find("\n]===]\n") != std::string::npos);
    CHECK(std::string(kShimName).rfind("!!!", 0) == 0);  // sorted before any addon's file
    if (const char *dump = std::getenv("GMOD_MCP_SHIM_DUMP")) {  // run by hand: luajit -bl on the file proves it parses
        std::ofstream(std::string(dump) + ".full.lua") << build_shim("print('hi') local x = [[a]]");
    }
}

static fs::path make_dir() {
    fs::path d = fs::temp_directory_path() / ("gmcp_el_" + new_token());
    fs::create_directories(d);
    return d;
}

static void test_sweep() {
    fs::path base = make_dir();
    put(base / "old1" / "early.lua", "a");
    put(base / "old2" / "root" / "lua" / "autorun" / "x.lua", "x");
    put(base / "fresh" / "early.lua", "x");
    put(base / "kept" / "early.lua", "x");
    put(base / "bad name" / "early.lua", "x");
    put(base / "user.txt", "u");
    auto old = fs::file_time_type::clock::now() - std::chrono::hours(48);
    std::error_code ec;
    for (const char *n : {"old1", "old2", "kept", "bad name"})
        for (fs::recursive_directory_iterator it(base / n, ec), end; it != end; it.increment(ec)) fs::last_write_time(it->path(), old, ec);
    for (const char *n : {"old1", "old2", "kept", "bad name"}) fs::last_write_time(base / n, old, ec);
    int n = sweep_dir(base.u8string(), 24, {"kept"});
    CHECK(n == 2 && !fs::exists(base / "old1") && !fs::exists(base / "old2"));
    CHECK(fs::exists(base / "fresh") && fs::exists(base / "kept") && fs::exists(base / "bad name"));  // young, listed, not a token
    CHECK(fs::exists(base / "user.txt"));
    CHECK(sweep_dir((base / "nothing").u8string(), 24, {}) == 0);
    fs::remove_all(base, ec);
}

void run_early_lua_tests(int *pass, int *fail) {
    test_tokens();
    test_shim();
    test_sweep();
    *pass = g_pass;
    *fail = g_fail;
}

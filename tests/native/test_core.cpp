// Unit tests of the module's tool helpers that need no game: Lua path lookup and the `fields` projection.
// Built as gmod-mcp-coretests (links the module's sources like gmod-mcp-snapshot), run by the CI.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "agent.h"
#include "tools.h"

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

int main() {
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

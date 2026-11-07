#include "resources.h"

#include "agent.h"

using nlohmann::json;

namespace resources {

std::string instructions() {
    const auto &c = agent::config();
    std::string s;
    s += "gmod-mcp drives a LOCAL Garry's Mod instance (mode " + std::string(agent::mode_name()) + ", instance '" +
         c.instance_id + "').\n";
    s += "Realms: 'menu' (always present in client mode, survives map changes), 'client' and 'server' (only exist once a map is "
         "loaded, destroyed/recreated on every map change). On srcds, only 'server' exists.\n";
    s += "Typical order: status -> map_load -> lua_exec / ents_find / trace / screenshot... -> errors_since / console_read.\n";
    s += "Pitfalls: (1) a missing realm returns the 'realm_unavailable' error: load a map; (2) everything is asynchronous on the game "
         "side (a console command or a spawn takes effect on the next frame): use wait_for(realm, condition) or wait_frames(n) "
         "instead of guessing; (3) the client realm is locked ('client_locked') if the game is connected to a non-loopback server; "
         "(4) if the game freezes (infinite loop), status reports it and game_terminate remains available.\n";
    s += "Lua files: working directory = " + c.tmp_dir +
         " (lua/mcpagent_tmp/). If you share this disk, write there directly then call lua_exec_file(realm, 'name.lua'); otherwise "
         "use lua_file_write. Errors then point to the real file.\n";
    s += "Documentation: api_search (ground truth: what actually exists in each realm), wiki_search / wiki_page "
         "(https://wiki.facepunch.com/gmod/). Full guide: resource mcpagent://guide.\n";
    s += "Multi-instance: instances_list; every tool accepts 'instance' to target another instance through the coordinator.";
    return s;
}

static std::string guide() {
    const auto &c = agent::config();
    std::string g = R"(# gmod-mcp - agent guide

## Realms
| realm | exists when | usage |
|---|---|---|
| menu | always (client mode) | loading maps, console, wiki/HTTP, error capture (OnLuaError) |
| server | map loaded (singleplayer/listen or srcds) | entities, players, traces, sounds, game logic |
| client | map loaded and lock allowed | rendering, screenshots, debug_draw, local player state |

The client and server states are destroyed and recreated on every map change: keep no references across maps.
The agent runtime (global table `__mcpagent`) is re-injected automatically.

## Workflow
1. `status`: available realms, map, fps, age of the main thread's last tick.
2. `map_load {map:"gm_construct"}` waits until client AND server are ready (timeout).
3. Iterate: `lua_exec`, `lua_exec_file`, `ents_find`, `trace`, `trace_through`, `screenshot_from`, `debug_draw`...
4. Check: `errors_since {cursor}` and `console_read {cursor}` (monotonic cursors; `lost` > 0 = lines lost).
5. Test: `tests_run {realm, path}` (GLuaTest format: `return { groupName=..., cases={ {name=..., func=function() expect(1).to.equal(1) end} } }`).

## Temporary Lua files
- Directory: `)" + c.tmp_dir + R"(`
- If your file system is the same as the game's, write there directly; otherwise use `lua_file_write {name, content}`.
- `lua_exec_file {realm, path}`: `path` relative to this directory (e.g. `test.lua`, `sub/a.lua`) or an absolute path.
- The chunk is named `@lua/mcpagent_tmp/<name>`, so errors and tracebacks point to the file; `include("mcpagent_tmp/x.lua")` works.

## Returned values
`lua_exec` returns all return values (`n` = exact count, `nil`s in the middle preserved), serialized to JSON:
Vector/Angle/Color/Entity/functions/cyclic tables have a `{"__type": ...}` form.
`print`/`Msg`/`MsgN`/`MsgC` emitted during execution are captured in `output`.

## Structured errors
Every tool returns `isError: true` and `{"error": {"code", "message", "details"}}`. Common codes:
`realm_unavailable`, `client_locked`, `main_thread_timeout`, `invalid_params`, `lua_error`, `timeout`, `not_found`.

## Limits
- No connection to remote servers; the client realm is refused outside singleplayer/listen/srcds loopback.
- The only outgoing network access is GET to wiki.facepunch.com (wiki_*).
)";
    return g;
}

static std::string tests_format() {
    return R"(# Test format (GLuaTest compatible)

A test file returns a table:

```lua
return {
    groupName = "My group",
    beforeAll = function(state) end,   -- optional
    beforeEach = function(state) end,  -- optional
    afterEach = function(state) end,   -- optional
    afterAll = function(state) end,    -- optional
    cases = {
        { name = "addition", func = function(state) expect(1 + 1).to.equal(2) end },
        { name = "expected error", func = function() expect(error, "x").to.err() end },
    },
}
```

Assertions: `expect(v).to.equal(x)`, `.to.beTrue()`, `.to.beFalse()`, `.to.beNil()`, `.to.exist()`, `.to.beA("string")`,
`.to.beValid()`, `.to.beInvalid()`, `.to.beGreaterThan(x)`, `.to.beLessThan(x)`, `.to.beBetween(a, b)`, `.to.aboutEqual(x, tol)`,
`.to.err()`, `.to.errWith("msg")`, `.to.succeed()`, `.to.haveLength(n)`, `.to.contain(v)`; negate with `.notTo` / `.toNot`.
Optional case fields: `skip = true`, `when = false`, `timeout`. `tests_run` returns for each case: status (pass/fail/error/skip),
message, line, duration.
)";
}

json list() {
    return json::array({
        json{{"uri", "mcpagent://guide"}, {"name", "guide"}, {"title", "gmod-mcp agent guide"},
             {"description", "Realms, workflow, temporary files, return formats, error codes."},
             {"mimeType", "text/markdown"}},
        json{{"uri", "mcpagent://tests-format"}, {"name", "tests-format"}, {"title", "Lua test format"},
             {"description", "GLuaTest format accepted by tests_run and list of assertions."}, {"mimeType", "text/markdown"}},
        json{{"uri", "mcpagent://config"}, {"name", "config"}, {"title", "Instance configuration"},
             {"description", "Absolute paths (game, Lua temporary directory, data), mode, port, instance identifier."},
             {"mimeType", "application/json"}},
    });
}

json templates() { return json::array(); }

json read(const std::string &uri) {
    const auto &c = agent::config();
    if (uri == "mcpagent://guide") return json{{"uri", uri}, {"mimeType", "text/markdown"}, {"text", guide()}};
    if (uri == "mcpagent://tests-format") return json{{"uri", uri}, {"mimeType", "text/markdown"}, {"text", tests_format()}};
    if (uri == "mcpagent://config")
        return json{{"uri", uri},
                    {"mimeType", "application/json"},
                    {"text", json{{"mode", agent::mode_name()},
                                  {"instance_id", c.instance_id},
                                  {"port", c.port},
                                  {"base_port", c.base_port},
                                  {"game_dir", c.game_dir},
                                  {"tmp_dir", c.tmp_dir},
                                  {"data_dir", c.data_dir},
                                  {"version", c.version}}
                                 .dump(2)}};
    return nullptr;
}

}  // namespace resources

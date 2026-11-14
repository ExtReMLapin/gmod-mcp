#include "resources.h"

#include <algorithm>

#include "agent.h"
#include "console.h"
#include "platform/platform.h"
#include "tools.h"
#include "util.h"

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
         "(4) if the game freezes (infinite loop), status reports it and game_terminate remains available; "
         "(5) every instance shares the game folder with the user's own game: archived convars (FCVAR_ARCHIVE) are saved to "
         "cfg/config.cfg and seen by every instance, and data/ and the cookies (sv.db/cl.db) are shared too: restore what a "
         "test changes; (6) entity names (targetname) are server-side: ents.FindByName does nothing on the client and "
         "Entity:GetName/SetName are server-only; (7) EyePos()/EyeAngles() are only reliable inside rendering hooks "
         "(use LocalPlayer():EyePos() elsewhere, e.g. in a lua_job); (8) a background test client may believe it has the "
         "focus (system.HasFocus() true), so fps_max_nofocus is not exercised there.\n";
    s += "Lua files: working directory = " + c.tmp_dir +
         " (lua/mcpagent_tmp/). If you share this disk, write there directly then call lua_exec_file(realm, 'name.lua'); otherwise "
         "use lua_file_write. Errors then point to the real file.\n";
    s += "Long Lua work (more than a few frames: scans, batches of traces, timed tests): do NOT loop inside lua_exec (the game is "
         "frozen until it returns, and a call longer than timeout_s fails with main_thread_timeout). Use lua_job: the script runs as a coroutine resumed every frame and MUST cooperate with the "
         "`job` object: job:step() in loops, job:progress(done, total, msg) to report progress (shown to the user), "
         "job:report(value) for intermediate results, job:sleep(s)/job:frames(n)/job:wait(fn), `return` for the result. "
         "Details: lua_job description, prompt write-lua-job.\n";
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
3. Iterate: `lua_check` (syntax and unknown globals before running anything; `path` takes a whole addon folder), `lua_reload` after editing an addon file (not include(): the client would run the copy sent at map load), `lua_exec`, `lua_exec_file`, `ents_find`, `trace`, `trace_through`, `screenshot_from`, `debug_draw`...
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

## Long-running Lua: lua_job
`lua_exec` runs in one go on the main thread (the game is frozen meanwhile; no time limit unless `budget`, so an infinite
loop freezes the game: the call fails with `main_thread_timeout` after `timeout_s`, `status` shows `main_thread_stalled`,
`game_terminate` ends it). The LuaJIT compiler is never turned off: code and the functions it defines run at real speed.
Returned values come back whole unless you pass `depth`/`limit`/`max_string` (the result then says `truncated`); a value
too big to send (`return _G`) fails with `result_too_large`. For anything that spans frames,
use `lua_job`: the script runs as a coroutine resumed once per frame and talks to the agent through `job`:

| call | effect |
|---|---|
| `job:progress(done, total, msg)` / `job:progress(msg)` | progress shown to the user (MCP progress, or task status) |
| `job:step()` | in loops: yields once this frame's slice (`slice_ms`, default 8 ms) is used |
| `job:yield()`, `job:frames(n)`, `job:sleep(s)` | resume on a later frame |
| `job:wait(fn, timeout)` | resume once `fn()` is true (checked every frame); false on timeout |
| `job:report(value [, channel])` | intermediate result: returned at the end, live through `reports_read` (`job:<id>`) |
| `args` (global) = `job.args`, `job:elapsed()` | call arguments, seconds since start |
| `return ...` | final result |

A resume that never yields freezes the game (optional `resume_limit_s`: best effort, stops loops written in the script
itself). Cancelling the request stops the job at the next frame; a map change destroys it (`realm_reset`). With
`timeout_s` >= 15 the call runs as an MCP task.

Globals the script assigns land in the realm's `_G` and the next job sees them: pass `fresh_env: true` to keep them in the
job's own table. `async: true` returns the job id at once: `lua_job_status {id, wait_s}` polls it and returns the result
once finished, `lua_job_cancel {id}` stops it; a job started while another runs in the same realm gets `concurrent_jobs`.
Pitfall: `EyePos()` / `EyeAngles()` are only reliable inside rendering hooks (wiki); a job runs outside them (agents saw
(0,0,0)): use `LocalPlayer():EyePos()` / `LocalPlayer():EyeAngles()`.

```lua
local found = {}
for i, ent in ipairs(ents.GetAll()) do
    if ent:GetClass() == "prop_physics" then found[#found + 1] = ent:EntIndex() end
    if i % 100 == 0 then job:progress(i, #ents.GetAll(), #found .. " props") end
    job:step()
end
return found
```

## Prompts and resources
Prompts (slash commands): `write-lua-job`, `debug-errors`, `run-tests`, `check-map`.
Resources: `gmod://errors` and `gmod://console` (subscribable), templates `gmod://wiki/{page}` and `gmod://lua/{path}`.

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
        json{{"uri", "gmod://errors"}, {"name", "errors"}, {"title", "Recent Lua errors"},
             {"description", "Last 100 Lua errors of every realm (JSON). Subscribable: updated when a new error is captured."},
             {"mimeType", "application/json"}},
        json{{"uri", "gmod://console"}, {"name", "console"}, {"title", "Game console"},
             {"description", "Last 200 console lines. Subscribable: updated when new lines are printed."}, {"mimeType", "text/plain"}},
    });
}

json templates() {
    return json::array({
        json{{"uriTemplate", "gmod://wiki/{page}"}, {"name", "wiki-page"}, {"title", "GMod wiki page"},
             {"description", "Raw markup of a page of https://wiki.facepunch.com/gmod/ (e.g. gmod://wiki/Entity:EmitSound). Cached 7 days."},
             {"mimeType", "text/plain"}},
        json{{"uriTemplate", "gmod://lua/{path}"}, {"name", "lua-tmp-file"}, {"title", "Temporary Lua file"},
             {"description", "A file of the agent's Lua working directory (lua/mcpagent_tmp/), e.g. gmod://lua/test.lua."},
             {"mimeType", "text/x-lua"}},
    });
}

bool subscribable(const std::string &uri) { return uri == "gmod://errors" || uri == "gmod://console"; }

// Version stamp of a subscribable resource: changes whenever its content changes.
uint64_t version(const std::string &uri) {
    if (uri == "gmod://errors") return console::errors_cursor();
    if (uri == "gmod://console") return console::cursor_now();
    return 0;
}

static std::string percent_decode(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2])) {
            out.push_back((char)std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

static json error(const std::string &code, const std::string &msg) { return json{{"__error", {{"code", code}, {"message", msg}}}}; }

static json read_dynamic(const std::string &uri) {
    const std::string wiki = "gmod://wiki/", lua = "gmod://lua/";
    if (uri == "gmod://errors")
        return json{{"uri", uri}, {"mimeType", "application/json"}, {"text", console::errors_since(0, 100).dump(2, ' ', false, json::error_handler_t::replace)}};
    if (uri == "gmod://console") {
        uint64_t now = console::cursor_now();
        std::string err, text;
        auto r = console::read(now > 200 ? now - 200 : 0, 200, "", err);
        for (const auto &l : r.lines) text += l.text + (l.text.empty() || l.text.back() != '\n' ? "\n" : "");
        return json{{"uri", uri}, {"mimeType", "text/plain"}, {"text", text}};
    }
    if (uri.rfind(wiki, 0) == 0) {
        std::string page = percent_decode(uri.substr(wiki.size()));
        if (page.empty() || page.size() > 200) return nullptr;
        json r = tools::wiki_page_tool(json{{"name", page}});
        if (!r.value("ok", false)) {
            if (r["error"].value("code", "") == "not_found") return nullptr;
            return error(r["error"].value("code", "error"), r["error"].value("message", ""));
        }
        return json{{"uri", uri}, {"mimeType", "text/plain"}, {"text", r["result"].value("content", "")}};
    }
    if (uri.rfind(lua, 0) == 0) {
        std::string err, path = tools::tmp_path(percent_decode(uri.substr(lua.size())), err);
        std::string body;
        if (path.empty() || !plat::read_file(path, body)) return nullptr;
        return json{{"uri", uri}, {"mimeType", "text/x-lua"}, {"text", body}};
    }
    return nullptr;
}

std::vector<std::string> complete(const std::string &uri_template, const std::string &arg, const std::string &value) {
    std::vector<std::string> out;
    std::string v = util::to_lower(value);
    auto starts = [&](const std::string &x) { return util::to_lower(x).rfind(v, 0) == 0; };
    if (uri_template == "gmod://wiki/{page}" && arg == "page") {
        std::vector<std::string> contains;
        for (const auto &p : tools::pagelist_from_cache()) {
            if (starts(p)) out.push_back(p);
            else if (!v.empty() && util::to_lower(p).find(v) != std::string::npos) contains.push_back(p);
        }
        out.insert(out.end(), contains.begin(), contains.end());
    } else if (uri_template == "gmod://lua/{path}" && arg == "path") {
        for (const auto &f : plat::list_files(agent::config().tmp_dir, true))
            if (starts(f)) out.push_back(f);
        std::sort(out.begin(), out.end());
    }
    return out;
}

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
    return read_dynamic(uri);
}

}  // namespace resources

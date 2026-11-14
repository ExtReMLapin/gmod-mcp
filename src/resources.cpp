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
         "focus (system.HasFocus() true), so fps_max_nofocus is not exercised there; (9) when your client runs map_load as a "
         "background task, wait for its result before calling a realm tool: an earlier call reaches the previous map's Lua "
         "state (every lua_exec/lua_job result has `generation`; map_load returns server_generation/client_generation).\n";
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
    s += "Multi-instance: instances_list; every tool accepts 'instance' to target another instance through the coordinator.\n";
    s += "Testing a branch of an addon: lua_mount {path: <your worktree>} (or instances_launch {lua_mount}) makes ONE game load "
         "Lua from that folder first; it takes effect at the next map_load, and the engine then names those files "
         "\"@lua/...\". Details: guide section \"Mounted Lua folder\".\n";
    s += "Feedback: while you work, note the frictions you meet with gmod-mcp itself (a wrong or unclear description, an error "
         "without a reason, a missing tool, a workaround you had to write). At the end of your task, only if one cost real "
         "time or a feature, or came back several times, offer the user in one line to report it to the maintainers: you do "
         "everything (draft, duplicate search, filing), it costs them nothing. The issue is PUBLIC: no secrets, tokens, "
         "personal paths, project or addon names, or project code; describe the context in general terms. Procedure: guide "
         "section \"Reporting frictions\" (repository https://github.com/ExtReMLapin/gmod-mcp).";
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
3. Iterate: `lua_check` (syntax and unknown globals before running anything; `path` takes a whole addon folder), `lua_reload` after editing an addon file (it runs the file again in every realm it belongs to; on a client connected to a separate server, include() would only find the copy that server sent), `lua_exec`, `lua_exec_file`, `ents_find`, `trace`, `trace_through`, `screenshot_from`, `debug_draw`...
4. Check: `errors_since {cursor}` and `console_read {cursor}` (monotonic cursors; `lost` > 0 = lines lost).
5. Test: `tests_run {realm, path}` (GLuaTest format: `return { groupName=..., cases={ {name=..., func=function() expect(1).to.equal(1) end} } }`).

## Temporary Lua files
- Directory: `)" + c.tmp_dir + R"(`
- If your file system is the same as the game's, write there directly; otherwise use `lua_file_write {name, content}`.
- `lua_exec_file {realm, path}`: `path` is absolute, or relative: looked up in this directory first (e.g. `test.lua`,
  `sub/a.lua`), then in the mounted Lua folder (lua_mount) if any, the game folder (`addons/x/lua/y.lua`), then `lua/`,
  `gamemodes/` and `addons/*/lua/` like include(). A miss lists every place tried. Same rule for `lua_job {path}`,
  `lua_check` and `lua_reload`; `tests_run` takes this directory or an absolute path only.
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
too big to send (over a million values or 64 MiB of strings) fails with `result_too_large`; a whole `_G` is
about 4.5 MB, which your client may save to a file instead of showing it: return only what you need. For anything that spans frames,
use `lua_job`: the script runs as a coroutine resumed once per frame and talks to the agent through `job`:

| call | effect |
|---|---|
| `job:progress(done, total, msg)` / `job:progress(msg)` | progress shown to the user (MCP progress, or task status) |
| `job:step()` | in loops: yields once this frame's slice (`slice_ms`, default 8 ms) is used |
| `job:yield()`, `job:frames(n)`, `job:sleep(s)` | resume on a later frame |
| `job:wait(fn, timeout)` | resume once `fn()` is true (checked every frame); false on timeout |
| `job:report(value [, channel])` | intermediate result: returned at the end, live through `reports_read` (`job:<id>`); each item is bounded (depth 8, 5000 entries, 1 MiB strings; cut parts are marked, `reports_truncated` counts them), 1000 items kept |
| `args` (global) = `job.args`, `job:elapsed()` | call arguments, seconds since start |
| `return ...` | final result |

A resume that never yields freezes the game (optional `resume_limit_s`: best effort, stops loops written in the script
itself). Cancelling the request stops the job at the next frame; a map change destroys it (`realm_reset`). With
`timeout_s` >= 15 the call runs as an MCP task.

Globals the script assigns land in the realm's `_G` and the next job sees them: pass `fresh_env: true` to keep them in the
job's own table. `async: true` returns the job id at once: `lua_job_status {id, wait_s}` polls it and returns the result
once finished, `lua_job_cancel {id}` stops it (ids look like `server-g3-1`: the number after `g` is the realm's generation, so after a map change an old id answers `realm_reset` instead of reaching a newer job); a job started while another runs in the same realm gets `concurrent_jobs`.
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

## Mounted Lua folder (lua_mount)
`lua_mount {path}` plugs a folder into ONE game process: typically the git worktree of an addon (the folder that
contains `lua/`). From then on every Lua lookup of that game looks in the folder FIRST, then in the game and its addons.
Other games (other test clients, the user's own game) are not affected. Windows game clients only (not srcds, not Linux).

Example. The worktree `D:/dev/my-addon-wt` is a copy of the addon installed as `garrysmod/addons/my_addon`.
`lua_mount {path: "D:/dev/my-addon-wt"}` (or `"D:/dev/my-addon-wt/lua"`: same result), then `map_load`:

| File on disk | What this game does with it | Source name (`debug.getinfo`) |
|---|---|---|
| `D:/dev/my-addon-wt/lua/autorun/my_addon.lua` | runs as the autorun file `autorun/my_addon.lua` at every map load (instead of the installed one) | `@lua/autorun/my_addon.lua` |
| `D:/dev/my-addon-wt/lua/my_addon/core.lua` | what `include("my_addon/core.lua")` loads; hides `garrysmod/addons/my_addon/lua/my_addon/core.lua` | `@lua/my_addon/core.lua` |
| `D:/dev/my-addon-wt/lua/my_addon/new.lua` (only in your branch) | `include("my_addon/new.lua")` finds it | `@lua/my_addon/new.lua` |
| `garrysmod/addons/my_addon/lua/my_addon/old.lua` (deleted in your branch) | STILL loaded from the installed addon if something includes it | `@addons/my_addon/lua/my_addon/old.lua` |
| `D:/dev/my-addon-wt/gamemodes/mygm/gamemode/shared.lua` | the gamemode file `mygm/gamemode/shared.lua` | `@gamemodes/mygm/gamemode/shared.lua` |
| `D:/dev/my-addon-wt/materials/...`, `sound/...`, `models/...` | NOT mounted: the game keeps the installed copies | - |
| `D:/dev/my-addon-wt/.git/`, `README.md`, anything outside `lua/` and `gamemodes/` | ignored | - |

Refused paths: a relative path, a folder without `lua/` (e.g. `D:/dev`, or a folder holding several addons), a folder
inside the game folder (`garrysmod/` or an installed addon: the engine already searches it), a path with `"`, `;` or a
line break.

When it takes effect: a Lua file is read when something loads it, and the game loads its addons at each map load. So:
set the folder, then `map_load`. Code already running stays as it was until it is loaded again. Removing it
(`path: null`) also applies from the next map load. `instances_launch {lua_mount}` starts a client with it from its first
map, then asks each client's engine what it really has: `instances[i].lua_mount` is `{requested, active, base_lua_mounted,
error?}` and a mount that is not active is a top-level `warning` with `isError: true` (never trust a test on that client).

Which file is used, per Lua path (e.g. `my_addon/core.lua`):
- in the folder (`<folder>/lua/my_addon/core.lua`): the folder's copy, even if the installed addon has one;
- only in the game or an addon: still loaded from there (the rest of the addon, other addons, the gamemode);
- deleted in your branch but still in the installed addon: STILL loaded, from the addon;
- `<folder>/gamemodes/` works the same way for gamemode files.

What sees the folder (checked live):

| Way code is loaded | With a mounted folder |
|---|---|
| autorun files, `include()`, `AddCSLuaFile` (the client gets the folder's copy) | folder first |
| console `lua_openscript` / `lua_openscript_cl` | folder first |
| `file.Read` / `file.Find` / `file.Exists` with `"LUA"` | see the folder's files (`"GAME"` does not) |
| gmod-mcp tools given a relative Lua path: `lua_exec_file`, `lua_job {path}`, `lua_check`, `lua_reload`, `lua_function_source` | folder first, same order as the engine; a miss lists the folder's paths too |
| materials, sounds, models, `data/` | not affected |

Names: the engine names a file of the folder relative to the folder's root. `debug.getinfo(f).source` is
`@lua/my_addon/core.lua`, not `@addons/my_addon/lua/my_addon/core.lua` as for the installed addon. Code that identifies
a caller by its file path sees another name. gmod-mcp's tools use the same names as the engine (`chunkname` overrides).

Editing a file of the folder: there is no autorefresh, saving a file does not reload it. To run an edit:
`lua_reload {path: "my_addon/core.lua"}` (runs the folder's copy again in its realms; what the file built at load time
is not reset) or `map_load` for a clean state. Check with `lua_function_source {realm, expr: "MyAddon.Update"}`: it
compares the running function with the folder's file (`matches_disk`). A `git checkout` in the worktree changes the
files at once; the running code changes only when it is loaded again.

Typical flow:
1. `instances_launch {count: 1, owner: "<you>", lua_mount: "D:/dev/my-addon-wt", map: "gm_construct"}` (or, on a client
   you already have: `lua_mount {path}` then `map_load`).
2. `lua_mount {}`: `lua_mount` is your folder, `base_lua_mounted: true`, the folder heads `search_paths.lsv` and `.lcl`.
3. `lua_function_source {realm: "server", expr: "MyAddon.Update"}`: `file` is in your folder, `matches_disk: true`.
4. Edit, then `lua_reload` or `map_load`, then test.
5. Done: `instances_close {owner}` (or `lua_mount {path: null}` then `map_load`).

If `base_lua_mounted` is false, the game's own `lua/` is missing from the server paths: remove the folder at once.

## Reporting frictions
The maintainers want to hear about every friction agents meet with gmod-mcp, without making work for the user.

1. While you work, note frictions with gmod-mcp itself: a description that was wrong or unclear, an error without a
   reason, a missing tool or option, a workaround you had to write that gmod-mcp could provide. Sub-agents report theirs
   to the agent that started them. Not frictions: your own mistakes, problems of the user's project or of the game.
2. Report only what matters: it cost real time or a feature, or it came back several times. At most one offer per task.
3. At the end of your task, in one line and in the user's language, offer to report it, and make clear it costs them
   nothing: e.g. "I hit 2 frictions with gmod-mcp (X, Y). Want me to report them to its maintainers? I do everything:
   draft, duplicate check and filing on GitHub."
4. If they agree, write the issue in English, PUBLIC-SAFE: no secrets, tokens, personal paths, user or machine names,
   project or addon names, or project code. Describe the context in general terms ("testing an audio addon on four test
   clients"). Use the template below.
5. Look for duplicates first, in https://github.com/ExtReMLapin/gmod-mcp/issues (open and closed): with `gh issue list
   --repo ExtReMLapin/gmod-mcp --state all --search "<keywords>"` if gh is available, or by fetching the issues page.
   If one exists, add a comment with your new facts instead of a new issue. No tool to search: skip this step.
6. File it:
   - if `gh auth status` succeeds: `gh issue create --repo ExtReMLapin/gmod-mcp --label agent-feedback --title "[agent] ..."
     --body-file <file>` (nothing for the user to click), then give them the link;
   - otherwise open (or give) this URL, filled in and URL-encoded, and tell them one click on "Submit new issue" is
     left: `https://github.com/ExtReMLapin/gmod-mcp/issues/new?labels=agent-feedback&title=<title>&body=<body>`
     (keep the body short enough for a URL, about 6000 characters).

Template (body):

```
## Context
<what you were doing, in general terms>
## What happened
<tool, arguments (shape only), what came back; how often; what it cost>
## What was expected
## Workaround used
## Suggestion
## Environment
- gmod-mcp module: <agent_version from status>
- gmod-mcp plugin / bridge: <if a version warning named another one>
- Platform: <win64/linux64, client or dedicated server>
- Coding agent: <Claude Code, Codex, OpenCode... and version if known>
- Model: <the model you run on>
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
                                 .dump(2, ' ', false, json::error_handler_t::replace)}};  // paths need not be UTF-8
    return read_dynamic(uri);
}

}  // namespace resources

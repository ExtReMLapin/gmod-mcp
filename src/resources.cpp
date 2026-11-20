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
    s += "Multi-instance: instances_list; every tool accepts 'instance' to target another instance through the coordinator. "
         "client_crashed / server_crashed = that game crashed (details.dump names the minidump, for a human): relaunch it with "
         "instances_launch and rerun what it was doing; instance_gone = it ended normally; instance_hung = frozen with no sign of life "
         "(close it, relaunch); instances_list state \"hung\" = main thread blocked for 60 s (180 s while loading), stalled_s and fps_low_s "
         "say how long; instances_launch refuses with low_memory when the machine cannot hold more clients (ignore_low_memory overrides).\n";
    // FP7137 (git grep FP7137: agent-facing text A1-A3, see docs/FP7137.md)
    s += "Launches and map loads: starting a game makes the other running games release their GPU resources for 2-10 s, and a "
         "map load ending then crashes that game (engine bug, Facepunch #7137). The bridge makes instances_launch and map_load "
         "wait for each other (a call that waited reports waited_s / waited_reason; errors map_load_in_progress / "
         "launch_in_progress: retry later or with a larger timeout_s). instances_launch also waits for a map load the bridge did "
         "not start (console, Lua, a script talking to the game: every game reports `loading` in instances_list), reason "
         "map_load_outside_bridge; map_load lists such games as other_games_loading. It cannot hold back a game started by "
         "hand or a load started by other means during a launch: avoid those. Other games can be unreachable for "
         "15-18 s after a game starts: wait and retry. Launch all clients in one instances_launch, then work. Guide section "
         "\"Launches and map loads\".\n";
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
line break, a path with non-ASCII characters, a UNC path (`\\server\share`, also `\\localhost\D$`), an extended-length or
device path (`\\?\D:\x`, `\\.\D:\x`) and a folder holding a file whose full path would pass Windows' 259
characters (MAX_PATH). The engine accepts all of these into its search paths and then loads nothing from them, so the
mount would report `active` while the game ran the installed code. For the non-ASCII, UNC and long cases a junction on a
short local path (`mklink /J C:\gmod-wt "<your folder>"`) is the way out.
A folder that REPLACES the game's own base Lua is refused too (error `shadows_base_lua`, the files are listed): any .lua of
the folder's `lua/` that also exists in the game's own `garrysmod/lua/`, or of its `gamemodes/base/` that exists in the game's
`gamemodes/base/` (e.g. `lua/includes/init.lua`: the client then hangs in its load and `map_load` times out). Move those
files out of the folder; `allow_shadow: true` (`lua_mount`, `instances_launch`) mounts it anyway. A `map_load` that times out
with a mount set names the mount in its message: look at `errors_since` and `lua_mount {}`.

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

Editing a file of the folder: there is no autorefresh, saving a file does not reload it. `status.lua_mount` tells what is
out of date: `stale_files` lists the folder's .lua files whose time differs from what each realm loaded (modified, added,
deleted; a `git checkout` that brings an OLDER file counts too), with the realms each runs in; `realms_without_mount`
names a realm that did not load with the folder in front (the folder was mounted or changed after that realm loaded, or the
load did not go through it: its code may be another copy than the folder's: `map_load`). `map_load` repeats the
check when it ends (`warning` and `lua_mount` in its result) and empties the engine's Lua file cache first. To run the edits:
`lua_reload {mounted_changed: true}` runs again every changed file of the folder in its realms (alphabetical order; deleted
files and entity/weapon/effect files are listed in `skipped`: `map_load`, or `lua_reload {path}` to force one), or
`lua_reload {path: "my_addon/core.lua"}` for a single file, or `map_load` for a clean state. A reload re-runs the file: it
does not undo what the old copy did (hooks or timers under another name, net strings, entities already spawned) and what
the file built at load time is not reset. If a file fails (a syntax error, a Lua error) `lua_reload {mounted_changed}` answers
the error `reload_failed` (`isError`): `details` holds the whole `reloaded` list, the other files did run.
Limit of `stale_files`: it compares file times, not contents. An edit that leaves the file's time as it was (a tool that
restores the time, or two saves inside the clock's resolution) is not seen, whatever its size; `map_load` still runs the
new code (the engine reads the files again, and the module empties its Lua cache first). Check with `lua_function_source {realm, expr: "MyAddon.Update"}`.

Which copy a function came from: the source name `@lua/my_addon/core.lua` is the same for the folder's copy and for the
installed copy (a Workshop addon is named that way too). `lua_function_source` and `lua_inspect` (on a function) say
which one the realm really ran: `from_mount` (true: the folder; false: the installed copy), `disk` (the real file),
`mount_has_file` (the folder has a file of that name now) and `provenance`: `mount_in_place_at_load` (the module put the
folder in front of this realm's Lua path right before the realm loaded, and the file was in the folder at that moment: recorded
at load time, not guessed from the name), `reloaded_from_mount` (the file was run again from the folder with lua_reload, whether it was loaded in place at load
or not), `mount_changed_since_load` (the realm loaded it from a folder that is no longer the mounted one: another folder was
mounted, or none, without a map load; `from_mount` is false, `loaded_from` is that folder and `disk` its copy when it
still exists: this is what the realm runs until `map_load`), `installed`, `not_on_disk` (inside a .gma), `no_file` (code from lua_exec/RunString). Limit: the engine, not the module,
reads each file, so `from_mount` comes from the folder's content when the realm loaded, not from a trace of each read.

Typical flow:
1. `instances_launch {count: 1, owner: "<you>", lua_mount: "D:/dev/my-addon-wt", map: "gm_construct"}` (or, on a client
   you already have: `lua_mount {path}` then `map_load`).
2. `lua_mount {}`: `lua_mount` is your folder and `base_lua_mounted: true` (the server realm finds the base Lua; null before a map is loaded).
3. `lua_function_source {realm: "server", expr: "MyAddon.Update"}`: `file` is in your folder, `matches_disk: true`.
4. Edit, then `lua_reload` or `map_load`, then test.
5. Done: `instances_close {owner}` (or `lua_mount {path: null}` then `map_load`).

Clients started by `instances_launch` carry `-mcp_test_client`. Their command line sets the archived convars `fps_max`,
`fps_max_nofocus` and `snd_mute_losefocus` (FCVAR_ARCHIVE: a clean quit saves them into `cfg/config.cfg`, which the
user's own game shares). Before such a client quits cleanly, the module sets each one back to the value `cfg/config.cfg`
has at that moment (the user's game may have written it since the launch), or to the convar's default when the file has
no line for it: `game_quit`, a `quit`/`exit` sent with `console_exec`, the main menu's Quit button and the window's close
button (the X, Alt+F4: the close request is held back 0.4 s while the values are put back) are covered. The same goes for
the archived convars changed through `instances_launch {fps_max, commands, launch_args}`: the bridge lists their names
on the command line (`-mcp_restore_convars a,b`) and the module restores those that are archived too (a convar changed
by `exec` of a cfg file is not tracked). Not covered: a
`quit` the user types by hand in that client's console, and a process killed from outside (which writes nothing). A
`convar_set` of one of these three on such a client is reverted at quit too. `instances_close` kills the process:
nothing is written. The same options on a game you start by hand are not restored: set them back yourself before it quits.

Clients started by `instances_launch` carry `-dxlevel 95`: with it the engine neither reads nor writes the user's video
settings (resolution, anti-aliasing), so test clients do not change the way the user's own game starts. A client you
start by hand with `-w`/`-h` does: add `-dxlevel 95` to it.

If `base_lua_mounted` is false, the game's own `lua/` is missing from the server paths: remove the folder at once.

`engine_serves_folder` (in `lua_mount {}` and `status.lua_mount`, once a map is loaded; per realm in `engine_serves`) is the
engine's own answer, not the module's bookkeeping: the module asks each realm's file system (`file.Exists` and `file.Time`
with `"LUA"`) for up to three files of the folder and compares the file time with the folder's. `false` means the engine lists
the folder but serves none of its files: the game runs the installed code, whatever `active` or `loaded_with_mount` say. A
`warning` says so, `map_load` repeats it, and `instances_launch {lua_mount}` reports that client's mount as `active: false`
with `error_code: "mount_not_effective"`. `null`: nobody could be asked (no map loaded yet). Remove the folder
(`lua_mount {path: null}`) and mount a plain local path.

A mounted folder that is renamed, moved or deleted (or whose `lua/` is deleted) serves nothing: `lua_mount {}`, `status.lua_mount`
and `map_load` then carry `folder_missing: true` and a `mount_folder_missing` warning (no list of "deleted" files), `lua_reload
{mounted_changed}` answers the error `mount_folder_missing`, and a realm that loaded while the folder was gone is not
`loaded_with_mount`. Restore the folder (or mount the right one) and `map_load`.

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
Calls to another instance (`instance` argument) can also fail with: `unknown_instance`, `coordinator_changing` (the game that
held the directory of instances ended and another is taking the role over, or has not heard from that instance yet: nothing
was sent, retry after `details.retry_after_s`; the plugin already waits a few seconds before giving it to you), `relay_lost` (the connection to the coordinator broke while the call was in flight and other games run: the call MAY HAVE BEEN EXECUTED, `details.maybe_executed`; check the game's state before repeating a call that is not idempotent), `instance_unreachable` (its process
runs but does not answer: frozen or busy; `status {instance}`, `game_terminate {instance}`), `instance_gone` (its process
ended normally: quit, closed or killed on request), `instance_hung` and `client_crashed` / `server_crashed` (see below).

## Reading results
Every result is one JSON object, sent twice with the same content: `structuredContent` and one text block. Some clients show
the model only `structuredContent` on success and only the text on an error (`isError: true`), so a hint is always a field
of that JSON, never prose around it. Every tool declares an `outputSchema` for its success result (an error is not checked against
it). Every successful result of the game has `answered_by` (the instance that answered). The hint
fields, all plain sentences for you, present only when they apply:
- `note`: how to read this result (a caveat, what a value means). `warning`: something is wrong or risky now and what to do.
- `next`: the call that goes on, as a sentence (`lua_job` start: poll with `lua_job_status`; `instances_list`: how to pick an instance).
- `next_call`: the same kind of hint as data, `{tool, arguments?, after_s?}`: the call to make next (wait `after_s` seconds first).
  In an error it is `error.next_call`. It is a suggestion, always next to its prose: check it fits what you are doing. Given for
  `realm_unavailable` (map_load), `main_thread_timeout` (status), `ambiguous_instance` (instances_list), `client_crashed` and
  `instance_gone` (instances_launch), `instance_unreachable` (status, later), `budget_exceeded` (the code as a lua_job), a refused
  launch/map load (`launch_in_progress`, `map_load_in_progress`: the same call later with a larger timeout_s), an argument typo
  (`invalid_params`: the corrected call), a started `lua_job` (lua_job_status), cut values (the call without the limits) and a
  wrong `lua_mount` on a launched client.
- `truncated_note`: values were cut by `depth`/`limit`/`max_string`; they are marked `__type = "truncated"`; raise the limit.
- `output_note`: the console lines in `output` are shared with the game; `fps_note`: a test client runs at most 20 fps.
- `version_warning`: the module in the game and the Claude Code plugin have different versions; tell the user.
- `waited_s` / `waited_reason`: the call waited for another launch or map load (see "Launches and map loads").
A number the game cannot hold is `null`, never a string: JSON has no NaN or infinity, so a non-finite value (NaN, +-infinity,
a coordinate or a "1e999" that overflows a double) reads `null` where a number is expected (`distance`, `pos`, `eye_ang`,
`number` of a convar...). Every number of every `outputSchema` accepts `null` for that reason: treat a null number as
"not a finite number", not as an error of the call.
An error is `{"error": {"code", "message", "details"}}`: branch on `code`, read `message` for what to do, `details` holds
the numbers. When the game is not running the plugin itself answers `game_not_running` in that same shape (`details`: `host`,
`port`, `reason`); start the game with `-mcp` and call again.

## A game crashed
`client_crashed` (a client) or `server_crashed` (a dedicated server) means the instance's process ended without a clean
shutdown. The answer's `details` hold `instance`, `pid`, `dump` (the minidump file name in `crashes/` of the game folder,
or null when none was found), `exit_code` (when known) and `evidence`. What to do: relaunch a client with
`instances_launch` (a server must be restarted by whoever runs it); what it held (map, Lua state, running tests) is lost,
so run the test again. The dump is for a human with a debugger: do not try to read it. When the dump could be read, `details`
also holds `fault` (where the exception happened, module+offset, e.g. `engine.dll+0x2069e0`) and `stack_candidates` (up to 8
words of the faulting thread's stack that point into a module, from the stack pointer up: a list of places to look at, not an
unwound call stack; the first ones are usually the callers): worth quoting in a bug report. A crash that happens only on
test clients can be timing: they run at `fps_max` 20, so several server ticks run per frame; relaunch with `fps_max: 300`
(`instances_launch {fps_max}`) and rerun to tell a timing bug (it disappears) from a plain one. A crash is told from a clean quit
by what the coordinator saw: a goodbye the game sends when it shuts down, a `game_quit`/`game_terminate` sent through it,
a dump written just before the process ended, then its exit code (0 = quit, 0xC0000005 and other 0xC... = crash). Limits:
a process killed from outside (Task Manager) with no dump looks like a crash with exit code 1 and `dump: null`; with
several games crashing together the dump is matched by time only (`dump_matched_by_time_only`). `instances_list` shows
a crashed instance for an hour as `state: "crashed"` with a `crash` object ({dump, exit_code, evidence, fault, stack_candidates}); a map_load in
flight when the client died answers `client_crashed` too (through `instances_launch {map}`: in that client's `map_error`).
A call in flight to an instance that dies is answered at once, not at its own timeout: the coordinator and, for the clients
it launched, the bridge watch the process. `details.memory` ({free_commit_mb, free_ram_mb}, measured when the crash is
reported) is added, with a sentence when the free commit is under 4 GB: out of memory is a likely cause (exit code
0xC000041D, no dump).

## A game is hung, or the machine is out of memory
- **Hung.** `instances_list` gives every instance `stalled_s` (seconds its main thread has been blocked, 0 when it ticks) and
  `fps_low_s` (seconds the frame rate has stayed under 10 fps, 0 when fine; not counted while it loads: measurements taken
  while it is above 0 are suspect). `fps` is counted over the last 3 s. After 60 s of blocked main thread (180 s while a map
  load is in progress) or of silence from a process that still runs, the entry is `state: "hung"` with a `note`: it will not
  recover by itself, close it with `instances_close {pids}` (or `game_terminate {instance}`) and relaunch; results measured on
  it since it stalled are void. `state: "unresponsive"` is the first stage: no heartbeat for 10 s but the process runs.
  A call in flight to an instance whose process runs but sent no heartbeat for 30 s fails with `instance_hung`
  (`details`: `instance`, `pid`, `silent_s`), and every later call to it answers `instance_hung` at once for as long as the process lives
  (`instance_gone` is only for a process that ended); a client whose heartbeats still come keeps the call waiting for its own timeout.
- **Out of memory.** `instances_launch` checks the machine before starting anything: free commit (virtual memory: RAM plus
  page file) must cover 1.6 GB per client plus 1 GB for the rest of the machine, free RAM 1.2 GB per client (a measured
  light client holds about 1.55 GB of commit and 1.2 GB of RAM). Otherwise it fails with `low_memory` and starts nothing:
  `details` = {free_commit_mb, free_ram_mb, running_clients (every session), per_client_mb, count, needed_commit_mb,
  needed_ram_mb}. Close clients you no longer use, start fewer, or pass `ignore_low_memory: true`.
- **Telling infrastructure from test results.** `low_memory`, `instance_hung`, `client_crashed`, `server_crashed`,
  `instance_gone`, `instance_unreachable`, `coordinator_changing`, `game_not_running` and `port_conflict` are about the
  machine or the games, never about the code under test: a harness should stop and report them, not count a failed test.

)";
    // FP7137 (git grep FP7137: agent-facing text A1-A3, see docs/FP7137.md)
    g += R"(
## Launches and map loads (several games)
Starting any Garry's Mod process makes every other running game release and restore its GPU resources for 2-10 s; the
main thread of those games is blocked meanwhile. A game whose map load ends inside that window crashes (engine bug,
Facepunch garrysmod-issues #7137).
- **What the bridge does.** It serialises `instances_launch` and `map_load` for every session that uses it (several
  Claude sessions and sub-agents included). `instances_launch` waits while any game loads a map. `map_load` (any target
  instance) waits while a game is being started and for 15 s after the last start. `instances_launch {map}` loads its
  maps after that delay, 2 at a time.
- **What you see.** A call that waited carries `waited_s` and `waited_reason` in its result (for `instances_launch {map}`:
  in each `instances[i]`; for a `map_load` run as a task: in its `statusMessage`) and reports progress while it waits.
  If the wait would go beyond the call's `timeout_s`, the call fails and nothing was started or sent: `map_load_in_progress`
  (a launch blocked by a map load) or `launch_in_progress` (a map load blocked by a launch or by the 15 s delay). Retry
  later, or with a larger `timeout_s`.
- **Loads the bridge did not start.** Every game tells the coordinator when it is loading a map (`loading: true` in
  `instances_list`, within a fraction of a second of the start), whatever started the load: `map` or `changelevel` in the
  console, Lua `RunConsoleCommand`, a script talking HTTP to the game, `map_load`. `instances_launch` waits for all of them
  (`waited_reason` starts with `map_load_outside_bridge`; same `map_load_in_progress` error at the timeout), and `map_load`
  lists the other games that were loading when it started (`other_games_loading`, it does not wait for them).
- **What it cannot do.** Hold back a game started by hand (not through `instances_launch`), a game without gmod-mcp (no
  `-mcp`), or a load started by the console or a script while a launch is already creating its processes or within 15 s
  after one: the bridge only delays its own calls. Do not do those while other games load or launch.
- **Unreachable after a launch.** Right after a game starts, the other games can be unreachable for 15-18 s
  (`instance_unreachable`, timeouts). That is expected: wait and retry. A dead game is `client_crashed`, not a timeout.
- **Order.** Launch every client you need in ONE `instances_launch` call (with `map` it loads 2 at a time after the
  settle delay), then work. Avoid interleaving launches with map loads, yours or another session's.

## UI input (ui_click, ui_key)
For what has no console command: the buttons of VGUI2 dialogs ("Disconnected by the server" > Close), Derma panels, the menu's
HTML. `ui_click {x, y}` takes the pixel of a `screenshot` image (coordinates are those of the image; pass `screenshot_size`
when it is not the game window's size, or `coords: "window"` for client-area pixels); `ui_key {key, text}` presses a key and
types text (Unicode included). Click a text field first, then type. `screenshot_after: true` returns the result of the click.
- The input is posted to the game's own window (PostMessage): the user's real mouse does not move and no window takes the
  focus. Windows game client only.
- Refusals, each its own error code: `unsupported` (srcds, not Windows), `not_a_test_client` (only games started by
  `instances_launch` accept input; `allow_user_game: true` overrides it), `client_locked`, `loading` (a map is loading),
  `window_unavailable` (minimized or not found), `cursor_hidden`, `out_of_bounds` (ui_click: outside the image or window).
- `cursor_hidden`: the game has the mouse (no menu or popup is open), so a click would be game input. The cursor counts as
  visible when the game menu is up (main menu, ESC menu, a dialog on it: `gui.IsGameUIVisible()`) or, in game, a Derma popup,
  the context or spawn menu has it (`vgui.CursorVisible()` in the client realm). `console_exec {cmd: "gameui_activate"}`
  opens the game menu.
- `ui_key` text: the engine narrows each typed character through the machine's ANSI code page with best-fit (CP1252: accents of
  western Europe arrive, `ł` becomes `l`, Cyrillic and CJK `?`). A text with such a character is refused (`text_not_representable`,
  details `code_page` and `characters`) unless `allow_lossy: true`; set it from Lua instead (`lua_exec` in the realm that owns the
  panel: `entry:SetText("...")` on a DTextEntry; `SetValue` does nothing while the entry has the focus a click gave it). A letter,
  a digit or space as `key` is typed as a character (`WM_CHAR`), not pressed.
- `screenshot` needs a loaded map: at the main menu (no client realm) there is no image to read coordinates from, and
  `screenshot_after` says so in `screenshot_note`. Use `coords: "window"` there.

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

# Test matrix

Each scenario is run through the MCP tools (curl or the Claude Code MCP client). Columns: ID, procedure, expected result,
last result (date). Statuses: ✅ green, ❌ red, ⏳ to run.

Procedure conventions: `call <tool> {args}` denotes a `tools/call`; "in game" = after `map_load gm_construct` (singleplayer).
"Persistence across map changes" scenarios are covered globally by **X-01**, which calls all three realms between
ten map changes, and per tool where relevant.


## Final campaign

Two consecutive full passes, each starting from a cold game start (ad-hoc harness, not part of the deliverable): the
functional scenarios of this matrix (session, Lua, console, world, rendering, sound, documentation, X-01 with 10 map
changes), the 75-check hardening battery, and the dedicated-server part (registration, `ambiguous_instance`, relay,
remote LAN machine refused, connection from a hosted game to the local srcds, net message srcds -> client, coordinator
re-election).

| Pass | Result | Date |
|---|---|---|
| A (cold start) | **140/140** ✅ | 2026-11-07 |
| B (cold start, after A) | **140/140** ✅ | 2026-11-07 |

Issues found by the campaign and fixed before these two passes:
- `connect` while the game hosts a map crashed the engine (access violation in `engine.dll`): `console_exec connect`
  now leaves the current game first.
- Back-to-back map transitions (disconnect + maxplayers + map) intermittently crashed the game's embedded Chromium
  (`html_chromium.dll`): `map_load` now waits for the menu's loading screen to close plus ~30 frames before a new load.
- An internal call was refused by the `ambiguous_instance` guard; the menu `status` spammed
  "Tried to look up command maxplayers as if it were a variable".

## Transport and security

| ID | Procedure | Expected | Last result |
|---|---|---|---|
| T-01 | `initialize` (2025-06-18) then `notifications/initialized` | returned protocolVersion = requested; notification → HTTP 202 | ✅ 2026-11-07 |
| T-02 | `tools/list` | all tools, `inputSchema` with `additionalProperties:false`, annotations | ✅ 2026-11-07 |
| T-03 | request without token / wrong token | HTTP 401 `unauthorized` | ✅ 2026-11-07 |
| T-04 | valid token + `Origin: http://evil.example`; `Origin: null` | HTTP 403 `origin_forbidden` | ✅ 2026-11-07 |
| T-05 | valid token + `Host: evil.example:27080` (DNS rebinding) | HTTP 403 `host_forbidden` | ✅ 2026-11-07 |
| T-06 | POST to the LAN address (192.168.1.10:27080) | connection refused (loopback bind) | ✅ 2026-11-07 |
| T-07 | `GET /mcp` | HTTP 405 | ✅ 2026-11-07 |
| T-08 | `server/discover` (2026-07-28 era, `MCP-Protocol-Version`/`Mcp-Method` headers) | `supportedVersions`, `io.modelcontextprotocol/tasks` extension, `resultType:"complete"` | ✅ 2026-11-07 |
| T-09 | 2026-07-28 era: `Mcp-Name` ≠ tool name | HTTP 400, `-32020` HeaderMismatch | ✅ 2026-11-07 |
| T-10 | 2026-07-28 era: unknown method | HTTP 404, `-32601` | ✅ 2026-11-07 |
| T-11 | version `2099-01-01` | HTTP 400, `-32022`, `data.supported` | ✅ 2026-11-07 |
| T-12 | 2026-07-28 era + Tasks extension: `map_load` | `resultType:"task"` then `tasks/get` → `completed` with `result` included | ✅ 2026-11-07 |
| T-13 | 2025-11-25 era: `tools/call` with `task:{}` on `map_load`, `tasks/get`, `tasks/result` | `CreateTaskResult{task}`; blocking `tasks/result` with `_meta.related-task` | ✅ 2026-11-07 |
| T-14 | `tasks/cancel` on a long `wait_for` task | status `cancelled`, the wait is interrupted | ✅ 2026-11-07 |
| T-15 | `tools/call` with invalid arguments (unknown property, wrong type) | `isError`, `invalid_params` | ✅ 2026-11-07 |
| T-16 | Claude Code: `claude mcp add --transport http …` then `claude mcp list` | "✔ Connected" | ✅ 2026-11-07 |
| T-17 | Claude Code (v2.1.319): `status`, `lua_exec` (3 realms), `trace_through`, `debug_draw`, `screenshot_from` from the MCP client | same results as curl; image delivered inline; `map_load` runs as an MCP Task (extension 2026-07-28), backgrounded by Claude Code and reported on completion | ✅ 2026-11-07 |
| T-18 | restart the game while Claude Code is connected; call a tool while the game is closed | next call after the restart works without `/mcp` (stateless server); while closed: immediate `ECONNREFUSED` | ✅ 2026-11-07 |

## Session and lifecycle

| ID | Procedure | Expected | Last result |
|---|---|---|---|
| S-01 | `status` at the main menu | client and server `available:false`, menu `runtime:true`, `map:null`, GMod version | ✅ 2026-11-07 |
| S-02 | `status` in game | map, gamemode, maxplayers, 3 realms available, client lock allows (singleplayer) | ✅ 2026-11-07 |
| S-03 | `status` during loading (concurrent call to `map_load`) | `loading:true`, `main_thread_stalled:false`, client absent | ✅ 2026-11-07 |
| S-04 | `status` during a freeze (`debug.sethook() while true do end`) | `main_thread_stalled:true`, `last_tick_age_s` > 5, `warning` | ✅ 2026-11-07 |
| S-05 | `map_list {filter:"gm_"}` / `{filter:"zzzz_rien"}` | `[gm_construct, gm_flatgrass]` / empty list | ✅ 2026-11-07 |
| S-06 | `map_load {map:"gm_construct"}` | client and server ready, `duration_s` ≈ 2–5 s | ✅ 2026-11-07 |
| S-07 | `map_load {map:"nexistepas"}` | immediate `not_found` error | ✅ 2026-11-07 |
| S-08 | 5 chained `map_load` | all OK, increasing generations | ✅ 2026-11-07 |
| S-09 | `map_load` maxplayers 1 then 2 then 1 | `game.MaxPlayers()` = 1 / 2 / 1; `SinglePlayer()` = true / false / true | ✅ 2026-11-07 |
| S-10 | `disconnect` | `disconnected:true`; `lua_exec` client/server → `realm_unavailable` | ✅ 2026-11-07 |
| S-11 | `game_quit` | clean shutdown (`Host_WriteConfiguration`, process exited) | ✅ 2026-11-07 |
| S-12 | `game_terminate` during an infinite loop | process killed from the network thread | ✅ 2026-11-07 |
| S-13 | `wait_for {condition:"true"}` | `satisfied`, 1 poll | ✅ 2026-11-07 |
| S-14 | `wait_for {condition:"false", timeout_s:1}` | `timeout` with `last_value` | ✅ 2026-11-07 |
| S-15 | `wait_for` during a `map_load` started in parallel | `realm_lost` | ✅ 2026-11-07 |
| S-16 | `wait_for` on an absent realm | `realm_unavailable` | ✅ 2026-11-07 |
| S-17 | `wait_frames {n:10}` | 10 frames | ✅ 2026-11-07 |
| S-18 | `events_wait {types:["menu_ready"], cursor:0}` after launch (curl `--retry-connrefused`) | returns as soon as the menu is ready | ✅ 2026-11-07 |

## Lua

| ID | Procedure | Expected | Last result |
|---|---|---|---|
| L-01 | `lua_exec "return 1, nil, 'a', nil"` | `n:4`, `[1,null,"a",null]` | ✅ 2026-11-07 |
| L-02 | Vector, Angle, Color, NULL, function, cyclic table, nan, inf | `{__type:…}` shapes, `cycle`, `nan`/`inf` | ✅ 2026-11-07 |
| L-03 | `print`, `MsgN`, then `error("boom")` | `lua_error` with trimmed traceback and captured `output` | ✅ 2026-11-07 |
| L-04 | syntax error | `lua_syntax_error` | ✅ 2026-11-07 |
| L-05 | server realm at the menu | `realm_unavailable` | ✅ 2026-11-07 |
| L-06 | `while true do end` with `budget:1` | `budget_exceeded` after ≈ 1 s, game intact | ✅ 2026-11-07 |
| L-07 | `lua_exec_file` on a file that `include`s another file | values + `output`, include resolved | ✅ 2026-11-07 |
| L-08 | `lua_exec_file` missing file / file with an error | `not_found` / traceback `lua/mcpagent_tmp/demo/err.lua:2` | ✅ 2026-11-07 |
| L-09 | `lua_file_write {name:"../evil.lua"}` | `invalid_params` | ✅ 2026-11-07 |
| L-10 | `lua_inspect _G depth 0 limit 5` (huge table) | `count` 2976, `truncated` | ✅ 2026-11-07 |
| L-11 | `lua_inspect Entity(1)` (userdata) | `Player` metatable, methods | ✅ 2026-11-07 |
| L-12 | `hook_list {event:"EvenementInexistant"}` | `known:false`, no hooks | ✅ 2026-11-07 |
| L-13 | `hook_trace PlayerSay 1 s` (never fired) | `calls:[]`, `tracer_removed:true` | ✅ 2026-11-07 |
| L-14 | `hook_trace Think max_calls 5` (high frequency) | 5 recorded, `total_calls` > 5, `capped` | ✅ 2026-11-07 |
| L-15 | after the trace, `hook.GetTable().Think` no longer contains the recorder | `tracer_removed:true` | ✅ 2026-11-07 |
| L-16 | `convar_get cvar_qui_nexiste_pas` | `not_found` | ✅ 2026-11-07 |
| L-17 | `convar_get rcon_password` | `protected` | ✅ 2026-11-07 |
| L-18 | `convar_set sv_gravity 500` then 600 | value read back, `previous` | ✅ 2026-11-07 |
| L-19 | `convar_set` on an `FCVAR_ARCHIVE` convar without `persist` | `archived` (no write to config.cfg) | ✅ 2026-11-07 |
| L-20 | `concommand_list {filter:"gm_spawn"}` | Lua + engine commands | ✅ 2026-11-07 |
| L-21 | `tests_run tests/selftest_pass.lua` | 5 pass, 1 skip, `ok:true` | ✅ 2026-11-07 |
| L-22 | `tests_run tests/selftest_fail.lua` | 1 pass, 1 fail (`expected 2 to equal 3`), 1 error (line 7) | ✅ 2026-11-07 |
| L-23 | `tests_run tests/selftest_setup_error.lua` | all cases in `error` "beforeAll: setup failed" | ✅ 2026-11-07 |
| L-24 | `lua_check` handler: GLua syntax (`!=`, `&&`, `!`, `//`, `continue`) passes; syntax errors in two files | one entry per file with its line (`unexpected symbol near '='` line 2, `'end' expected` line 4) | ✅ 2026-11-08 (Lua runtime, menu) |
| L-25 | `lua_check` handler: typo `prnit(...)`, `leaked = 1` in a nested function, a global defined in file A and used in file B, `ENT.Type` | `unknown_globals` prnit (line 5); `globals_written` leaked (line 9); nothing for the cross-file global nor ENT | ✅ 2026-11-08 (Lua runtime, menu) |
| L-26 | `lua_check` handler on a 70 000-line chunk with table constants, 64-bit numbers and a nested function | globals found on lines 300, 69999 and 70002 (4-byte line info), ≈ 55 ms | ✅ 2026-11-08 (Lua runtime, menu) |
| L-27 | `lua_check {path:"<folder>"}` on a folder of `.lua` files; `{code}` and `{path}` together; missing path | `file` = `lua/mcpagent_tmp/...` for each file; `invalid_params`; `not_found` | ✅ 2026-11-11: 3 files, the syntax error with its file and line, `prnit` unknown, `SHARED_X` written in one file and read in another not reported; code+path → invalid_params; missing → not_found |
| L-28 | `lua_exec "local x = = 1"` | `lua_syntax_error` with `details.line` 1 | ✅ 2026-11-11: `details.line` 1 |
| L-29 | `lua_reload {path:"autorun/<file>.lua"}` for a shared addon file edited after the map loaded; then `cl_`, `server/` and an ambiguous name | runs in server then client, the new code is live on both; `cl_` → client only, `server/` → server only; ambiguous → first match + `other_matches` + `warning`; unknown → `not_found` | ✅ 2026-11-11 (files in lua/mcpagent_tmp): shared → server + client, `cl_` → client, `server/` → server, unknown → not_found; edited on disk then reloaded → v2 on both sides |
| L-30 | `lua_exec` loop benchmark with and without `jit: true`; a function defined by a `jit: true` chunk then timed from a timer | `jit: true` several times faster; no `budget_exceeded` possible; default keeps the budget | ✅ 2026-11-11: same loop 249 ms default, 48 ms with jit; a function defined by a jit chunk timed later 55 ms; default budget still interrupts |
| L-31 | `lua_job {jit: true}` | runs, no resume limit, result returned | ✅ 2026-11-11 (menu realm): 55 ms with jit, 11.6 s spread over frames without |
| L-32 | `console_exec {command: "echo hi"}` (wrong name) | `invalid_params` naming `command`, suggesting `cmd`, listing the accepted arguments | ✅ 2026-11-11: "Unknown argument 'command' (did you mean 'cmd'?)", missing and accepted arguments listed |
| L-33 | client started with `-multirun ... +map gm_construct` from a background script while another application keeps the focus (cursor parked at 40,40 eight times, read back after 150 ms) | the game does not pull the cursor back; console line `told it the focus was lost`; clicking the game still captures the mouse normally | ✅ 2026-11-11 green, new DLL, same conditions as the red run: HasFocus false, pulled back 0/8, the guard's console line present |

## Console and errors

| ID | Procedure | Expected | Last result |
|---|---|---|---|
| C-01 | `console_exec "echo bonjour_mcp"` (menu: blocked in Lua) | `blocked_in_lua`, `via:"engine"`, output captured | ✅ 2026-11-07 |
| C-02 | `console_exec` client realm, blocked command | engine fallback, output captured | ✅ 2026-11-07 |
| C-03 | `console_exec "path"` (multi-line output) | several dozen lines | ✅ 2026-11-07 |
| C-04 | `console_exec "connect 192.168.1.99:27015"` (another machine) | `forbidden` (never a remote server) | ✅ 2026-11-07 |
| C-05 | `console_read {regex:"bonjour_mcp"}` | filtered lines | ✅ 2026-11-07 |
| C-06 | `console_read` with a cursor older than an overflow (reduced capacity) | `lost` > 0 | ✅ 2026-11-07 |
| C-07 | one error in each of the 3 realms (`timer.Simple(0, error)`) then `errors_since` | 3 errors, correct realm, stack | ✅ 2026-11-07 |
| C-08 | `errors_clear` then `errors_since {cursor:0}` | empty list, `lost:0` | ✅ 2026-11-07 |

## World, rendering, sound, performance (phase 4)

| ID | Procedure | Expected | Last result |
|---|---|---|---|
| W-01 | `ents_find {class:"classe_inexistante"}` | empty list | ✅ 2026-11-07 |
| W-02 | `ents_find {class:"info_player_start", limit:3}` / `near`+`radius` | `count` 33, 3 returned, `truncated`; sorted by distance | ✅ 2026-11-07 |
| W-03 | `ent_info {id:99999}` | `not_found` | ✅ 2026-11-07 |
| W-04 | door: `ent_spawn prop_door_rotating` (gm_construct no longer has a door in this build), `ent_fire Open`, `wait_for m_eDoorState==2` | `closed` → `open` | ✅ 2026-11-07 |
| W-05 | `ent_spawn {class:"classe_bidon_xyz"}` | `invalid_class` | ✅ 2026-11-07 |
| W-06 | `ent_remove` then `ent_info` | `not_found` | ✅ 2026-11-07 |
| W-07 | `player_set {pos, ang, noclip, god}` then check on the client side | position/angles/noclip identical on client and server | ✅ 2026-11-07 |
| W-08 | `bots_add {n:1}` with maxplayers 1 | `no_slots` | ✅ 2026-11-07 |
| W-09 | `bots_add {n:2}` with maxplayers 4, then `{n:5}` | 2 bots added; `no_slots` | ✅ 2026-11-07 |
| W-10 | `trace` toward the sky / at a known wall | `hit_sky`; `plaster`, `GM_CONSTRUCT/WALL_BOTTOM`, normal | ✅ 2026-11-07 |
| W-11 | `trace_through` single wall (704,132,-103 → 1400.6,534.2,-103) | 1 surface, thickness 23.6, plaster → concrete | ✅ 2026-11-07 |
| W-12 | `trace_through` two walls (→ 1217.3,269.5,-103) | 2 surfaces (86.1 and 49.8) | ✅ 2026-11-07 |
| W-13 | `trace_through` with no obstacle | 0 surfaces | ✅ 2026-11-07 |
| R-01 | `screenshot` at the main menu | `realm_unavailable` (the menu has no render.Capture) | ✅ 2026-11-07 |
| R-02 | `screenshot` in game | non-empty 1280×720 JPEG (visually checked) | ✅ 2026-11-07 |
| R-03 | `screenshot {w:320,h:240,format:"png"}` | 320×240 PNG (render target) | ✅ 2026-11-07 |
| R-04 | `debug_draw` (box, line, text, sphere) then `screenshot_from` | primitives visible; `camera_restored:true`, no CalcView hook left | ✅ 2026-11-07 |
| R-05 | `debug_clear` then `screenshot_from {w:480,h:270}` | primitives absent | ✅ 2026-11-07 |
| R-06 | `debug_draw {type:"cube"}` | `invalid_params` | ✅ 2026-11-07 |
| N-01 | `sound_emit {name:"son/inexistant.wav"}` | `not_found` | ✅ 2026-11-07 |
| N-02 | `sound_trace` during a `sound_emit` (level 80, pitch 120, volume 0.5, dsp 1) | one call, same parameters, `tracer_removed` | ✅ 2026-11-07 |
| P-01 | `perf_snapshot` at the menu / in game | fps, frametime, Lua memory of the menu / of the 3 realms, entity count | ✅ 2026-11-07 |
| P-02 | `perf_hook_cost Think` with an artificially expensive hook (2 ms) | ranked first, functions restored | ✅ 2026-11-07 |

## Documentation (phase 4)

| ID | Procedure | Expected | Last result |
|---|---|---|---|
| D-01 | `api_search util.TraceLine` / `ents.Create` | realms client+server / server only, wiki link | ✅ 2026-11-07 |
| D-02 | `api_search Vector:Distance` (3 realms) / `Entity:EmitSound` (method) | realms client, server, menu / client, server | ✅ 2026-11-07 |
| D-03 | `api_search zzzinexistant` | 0 results | ✅ 2026-11-07 |
| D-04 | `wiki_search EmitSound` cold cache then `util.TraceLine` warm cache | `cache:"miss"` (≈0.9 s) then `"hit"` (≈0.09 s), exact match first | ✅ 2026-11-07 |
| D-05 | `wiki_search trclne` (fuzzy) | `util.TraceLine` first | ✅ 2026-11-07 |
| D-06 | `wiki_page Entity:EmitSound` (text) / `GM:EntityEmitSound` (json) | markup / JSON | ✅ 2026-11-07 |
| D-07 | `wiki_page Page_Qui_Nexiste_Pas_42` | `not_found` | ✅ 2026-11-07 |
| D-08 | network down (`MCPAGENT_WIKI_OFFLINE=1`) with stale cache / without cache | `cache:"stale"` / `network_error` | ✅ 2026-11-07 |

## Hardening (scripted battery, 75 checks)

Run with an ad hoc test harness that is not part of the deliverable. After each group, the game is checked to still respond
(`status`, `main_thread_stalled:false`). Last result: **75/75 ✅ 2026-11-07**.

| Group | Scenarios |
|---|---|
| Protocol (H-01…H-17) | empty body / invalid JSON / non-object JSON / batch / 100,000 nested arrays (rejected before parsing) / arguments too deep / string id / non-string tool name / arguments as an array / jsonrpc 1.0 / `Host: 127.0.0.1:27080.evil.com` / `Host` without port / whitespace around the token (OWS, RFC 9110) / lowercase `bearer` scheme / 40 MiB body (413) / 20 half-open connections that do not block the server / unknown paths (404) |
| Lua payloads (H-20…H-33) | binary bytes and invalid UTF-8 / 200,000-element table / table nested 5000 levels deep / 50 MiB string / 300,000 `print` calls (output capped at 1 MiB) / `__tostring` that raises an error / error with a table value / `yield` at the top level / reentrant call into the pump / runtime wiped (reinjected) / `print` removed / very long expression / invalid 5 MiB source / `inspect _G` depth 8 |
| Files (H-40…H-43) | Windows reserved names (`con`, `NUL`, `aux`, `com1`), traversals (`..`, `C:/`, UNC), trailing dots/spaces, empty name, 300-character name / 8 MiB file / DLL executed as Lua |
| Concurrency (H-50…H-54) | 200 parallel `lua_exec` without mixed-up results / `status` responsive during 30 long-polls / two concurrent `map_load` (`busy`) / 30 calls during a map change (clean errors only) / more than 32 active Tasks refused |
| Limits (H-60…H-70) | 7680×4320 capture refused / 5000 debug primitives / invalid regex / catastrophic regex / event cursor in the future / capped `Think` trace / `while true` condition stopped by the budget / `tests_run` on `.exe` files / `Kill` on the player / `connect 10.0.0.1` / `echo a; connect 8.8.8.8` |
| Endurance (H-80, H-81) | 3000 varied calls in 4 s on 8 threads: process memory 1030 → 897 MiB, Lua memory stable after GC |

Flaws found and fixed by this battery:
- non-numeric `Host` port accepted;
- stack overflow on deeply nested JSON;
- HTTP thread pool exhaustible by long-polls;
- concurrent `map_load`;
- unbounded captured output;
- Windows device names;
- oversized captures;
- **`connect` chained after `;`, which bypassed the network guard**;
- empty 500 on non-object JSON.

## Installation and loader (self-install)

| ID | Procedure | Expected | Last result |
|---|---|---|---|
| I-01 | pristine `menu.lua`, zip files only, start with `-mcp +map gm_construct` | module loaded by the server autorun, attaches to the menu state, writes the loader block (`installed`), all 3 realms work | ✅ 2026-11-07 |
| I-02 | restart with `-mcp` (no map) | module loads at the main menu, `menu.lua` not rewritten | ✅ 2026-11-07 |
| I-03 | old block (v1) present | rewritten in place (`updated`), CRLF line endings kept | ✅ 2026-11-07 |
| I-04 | `mcpagent_uninstall` | `menu.lua` byte-identical to the original backup | ✅ 2026-11-07 |
| I-05 | crash sentinel `data/mcpagent/load_in_progress.txt` present | module skipped with a message, menu works | ✅ 2026-11-07 |
| I-06 | garbage / empty DLL | skipped with a message, no Windows "bad image" dialog, menu works | ✅ 2026-11-07 |
| I-07 | start without `-mcp`, with a map | nothing written, module inactive | ✅ 2026-11-07 |
| I-08 | release autorun stamped with the right / a wrong CRC32 (srcds) | module loaded / refused with "does not match this release" | ✅ 2026-11-07 |
| I-09 | several instances, call without `instance` | `ambiguous_instance` listing the ids; every answer carries `answered_by` | ✅ 2026-11-07 |
| I-10 | `console_exec connect` to a closed port / to the local srcds | `connect_result.status` = `failed` with the reason and a note about the in-game dialog / `connected` | ✅ 2026-11-07 |

## MCP 2026-07-28 features (v0.2, scripted battery, 63 checks)

Run with a test client speaking both protocol eras (headers `MCP-Protocol-Version`, `Mcp-Method`, `Mcp-Name`), in the
main menu.

| ID | Procedure | Expected | Last result |
|---|---|---|---|
| M-01 | `server/discover`, `tools/list` | capabilities prompts, completions, resources.subscribe; `serverInfo` (with icon) in every result's `_meta`; `outputSchema` on tools; `x-mcp-header: Instance` on `instance` | ✅ 2026-11-07 |
| M-02 | list with a cursor; `resources/read` unknown URI (modern / legacy) | `-32602` / `-32602` / `-32002`; `ttlMs` + `cacheScope` on reads | ✅ 2026-11-07 |
| M-03 | `Mcp-Param-Instance` differing from `arguments.instance` | HTTP 400, `-32020` | ✅ 2026-11-07 |
| M-04 | `prompts/list`, `prompts/get` (each prompt, missing argument, unknown prompt, legacy era) | 4 prompts; text includes the `job` protocol; `-32602` on errors | ✅ 2026-11-07 |
| M-05 | `completion/complete`: map, realm, temporary Lua file, wiki page, unknown ref | `gm_construct`, `server`, the file, `Entity:EmitSound…`, `-32602` | ✅ 2026-11-07 |
| M-06 | resource templates `gmod://lua/{path}` (incl. `../` traversal), `gmod://wiki/{page}`, `gmod://errors`, `gmod://console` | contents returned; traversal refused | ✅ 2026-11-07 |
| M-07 | `subscriptions/listen` without SSE / with SSE on console + errors + a non-subscribable URI, then print and raise an error | error / acknowledged first with the honored filter and `subscriptionId`, then `notifications/resources/updated` for both | ✅ 2026-11-07 |
| M-08 | `lua_job` with `progressToken` over SSE (60 frames, `job:progress`, `job:report`, `print`) | `text/event-stream`; increasing progress with total 60; result, 3 reports and output | ✅ 2026-11-07 |
| M-09 | `lua_job`: plain JSON, runtime error, endless loop (`resume_limit_s` 0.5), `job:step` over 3M iterations, syntax error, code + path | result / error with traceback / aborted "without yielding", game responsive / spread over several frames / `lua_syntax_error` / refused | ✅ 2026-11-07 |
| M-10 | client closes the SSE stream of a running job | the coroutine stops (global counter frozen) | ✅ 2026-11-07 |
| M-11 | legacy `notifications/cancelled` for a running `wait_for` | 202; the call returns `cancelled` within a second | ✅ 2026-11-07 |
| M-12 | `lua_job` with `timeout_s` 60 (Tasks extension) then `tasks/get`, and `tasks/cancel` on another one | task; `statusMessage` "phase i (i/40)"; completed with result; cancelled job stops | ✅ 2026-11-07 |
| M-13 | `convar_set` on an archived convar (`fps_max`) with the elicitation capability: decline, then accept | `input_required` form elicitation / `archived` error / value applied (restored afterwards); without the capability or in legacy era: `archived` error | ✅ 2026-11-07 |

## Bridge and plugin (gmod-mcp-bridge)

`tests/bridge/test_bridge.py <bridge> [a] [f] [m] [fslow] [b] [c]`. Cases a, f, m and multi (the default) need no game and run in CI on both
platforms; f and m use a fake gmod-mcp server run by the script. Mutation check: the bridge of 319a536 (before the
fixes below) fails F cancel and dies on F non-string version, the current one passes everything.

| ID | Check | Expected | Result |
|---|---|---|---|
| B-1 | A: nothing listening on the port: `initialize`, `tools/list`, `tools/call status`, `ping` | answered by the bridge; `tools.listChanged` true; cached or fallback list; call → `isError` + `game_not_running` | ✅ 2026-11-11 (Windows) |
| B-2 | B: live game, legacy era: `initialize`, `tools/list`, `instances_list`, `wait_frames n=240` with a progressToken | game's instructions and >40 tools relayed; progress notifications arrive before the result | ✅ 2026-11-11 (Windows) |
| B-3 | B: modern era (`_meta` protocolVersion 2026-07-28): `server/discover`, `tools/call status` | headers derived by the bridge accepted; `resultType` complete | ✅ 2026-11-11 (Windows) |
| B-4 | C: game appears mid-session (local relay started after the first list), then leaves | `notifications/tools/list_changed` within 2 s; live list; after it leaves, calls say `game_not_running` | ✅ 2026-11-11 (Windows) |
| B-11 | F (fake): progress order; cancel of a silent stream; stream ending without a response; 8 MB result; 30 concurrent calls with string ids; non-ASCII header values (base64 sentinel); non-string `_meta` version; invalid JSON, batch, blank line, `initialize` without params; stdin closed during a call; rejected token; version mismatch; game appearing mid-session | progress 1,2,3 before the result; connection closed < 1 s, no response, notification forwarded (legacy); JSON-RPC error; intact; every id answered; decodes back; answered, bridge alive; -32700 / -32600 / ignored / answered; exit < 3 s; message names the token; instructions start "Version mismatch"; list_changed | ✅ 2026-11-11 Windows + Linux (WSL), 25 checks |
| B-12 | M (fake, 2026-07-28 client): offline `server/discover`, list, call; missing `clientCapabilities`; unsupported version; `subscriptions/listen` (tools + prompts) while closed then the game appears; resource subscription honored subset, relayed, reopened after a game restart, cancelled; cancel of a call | `resultType` complete + serverInfo, `ttlMs` 0; -32602; -32022 with `supported`; acknowledged first with the subscription id; list_changed only on the subscription, none outside; only `gmod://` URIs; updates carry the id; reopened; game stream closed, nothing more; stream closed, no notification forwarded; tool annotations kept, fallback `status` read-only | ✅ 2026-11-11 Windows + Linux (WSL), 16 checks |
| B-13 | B/C/C2 (live game, old module): listen through the bridge to the real game; 2026-07-28 client while the game appears | `gmod://console` honored, `gmod://nope` not; list_changed on the subscription; live list | ✅ 2026-11-11 Windows |
| B-14 | fslow: stream silent for 65 s | result delivered (no idle limit on client requests) | ✅ 2026-11-11 Windows (65 s) |
| B-16 | multi: 6 bridges sharing one config folder (3 legacy sessions, 3 sessions of 2026-07-28 with a subscription), game appears, then 18 concurrent list requests | each bridge tells its session (plain notification or on the subscription); shared cache valid JSON with both eras' entries; no temporary file left | ✅ 2026-11-11 Windows |
| B-17 | orphan: the launcher of the bridge dies without closing the pipe; control with `--no-parent-watch` | bridge exits within 5 s; stays alive with the option | ✅ 2026-11-11 Windows + Linux (WSL) |
| B-18 | fuzz: 400 random messages (random shapes, ids, params, `_meta`, invalid UTF-8 lines), fake game up | bridge alive, still relays, every stdout line a JSON-RPC 2.0 message | ✅ 2026-11-11 Windows + Linux (WSL) |
| B-19 | multi repeated 5 times (cross-process lock around the shared cache) | 5/5; without the lock an entry was lost once (read-merge-write race) | ✅ 2026-11-11 Windows |
| B-20 | A: bridge that never saw the game, empty config folder | full tool list (55), prompts, resources and the module's instructions from the snapshot embedded at build time, prefixed "The game is not running", no machine path | ✅ 2026-11-11 Windows + Linux (WSL) |
| B-21 | Live game tool list = embedded snapshot (same release) | same names and schemas | ✅ 2026-11-11: 55 tools, identical in both eras |
| B-22 | End to end in Claude Code 2.1.322: plugin installed from a local marketplace, session started with the game closed, then the game started | tools listed while closed, `status` says the game is not running; after the game starts, `status` answers through the bridge with no /mcp reconnect | ✅ 2026-11-11 |
| B-23 | End to end with the published 0.3.0 plugin: marketplace add + install (archive from the release, SHA-256), new session, module installed from the plugin as the skill says, game started, status through the bridge, instances_launch + instance_select + instances_close | everything works; **bug**: on Windows the skill's `gmod-mcp-bridge --module-dir` ran the Linux binary (Exec format error), fixed in 0.3.1 | ✅ 2026-11-11 (bug fixed in 0.3.1) |
| B-15 | Module: 2026-07-28 request without `_meta` protocolVersion or `clientCapabilities` | -32602, HTTP 400 | ✅ 2026-11-11 (conformance suite, new DLL) |
| B-5 | Plugin assembled like CI (skill, `.mcp.json` with `${CLAUDE_PLUGIN_ROOT}/bin/gmod-mcp-bridge`, bridge, module), installed in an isolated Claude Code 2.1.322 | `claude plugin validate` passes; `claude mcp list`: `plugin:gmod-mcp:gmod ✔ Connected`, also with nothing on the port | ✅ 2026-11-11 (Windows) |
| B-6 | Marketplace `archive` source pointing at a GitHub release asset (redirected download) | downloaded and unpacked (refused only because that zip is not a plugin) | ✅ 2026-11-11 |
| B-7 | Command without `.exe` in `.mcp.json` on Windows | `fb` resolves to `fb.exe`: one `.mcp.json` for both platforms | ✅ 2026-11-11 |
| B-8 | Version mismatch: module in the game older than the bridge | instructions start with "Version mismatch ..." | ⏳ needs a module of another version in the game |
| B-9 | Linux: build of the bridge, `--module-dir`, case A | builds without warnings, 5/5 | ✅ 2026-11-11 (WSL Ubuntu 22.04, g++ 11; a full build at -j 16 leaves ≥ 6.8 GB free) |
| B-10 | Real update cycle: 0.3.0 installed, 0.3.1 published, marketplace refreshed | `claude plugin marketplace update` + `claude plugin update`: 0.3.0 → 0.3.1, installed in its own folder next to 0.3.0 (the running bridge is not replaced; "restart to apply"); module then installed with the fixed skill command from the 0.3.1 plugin | ✅ 2026-11-11 (manual update; the auto-update toggle uses the same path, not exercised) |

## Test clients (instances_launch) and light instances

Measured 2026-11-11 on gm_construct, one client at a time (16-core PC; CPU in % of one core, the game and its 4
browser subprocesses; private memory):

| Options | CPU | Memory | Note |
|---|---|---|---|
| 640x360, 30 fps (the old skill line) | 24.1 % | 1465 MB | |
| 640x360, 20 fps | 19.1 % | 1475 MB | **light preset** |
| 640x360, 20 fps, low graphics (-dxlevel 90, mat_picmip 2, shadows/3D sky/detail props/specular off, no HUD) | 18.4 % | 1338 MB | graphics settings are saved into the user's own: not offered |
| 480x270, 20 fps, low graphics | 17.8 % | 1325 MB | |
| 640x360, 20 fps, low graphics, -nosound | 16.2 % | 1336 MB | `sound: false` |
| 640x360, 20 fps, -textmode | 17.7 % | 1470 MB | no gain: not offered |

Crashes: 320x240 and 480x270 with the default HUD crash while loading a map (integer division by zero in client.dll at
client Lua startup; 5 dumps); the same sizes survived with the HUD off. Minimum width: 640.
**Side effect**: any client started with `-w/-h` saves that size as the game's video setting (registry
`HKCU\Software\Valve\Source\garrysmod\Settings`) as soon as it starts, even when closed by game_terminate. Documented in
the tool description, the skill and the README; the bridge does not touch the user's settings.

| ID | Check | Expected | Result |
|---|---|---|---|
| I-11 | session: bridge tools listed offline and live; default instance injected and mirrored in Mcp-Param-Instance; explicit instance wins; null clears; close refuses foreign instances; bad game_dir refused | as described | ✅ 2026-11-11 (11 checks) |
| I-12 | launch (live): 2 light clients with gm_construct, default instance, close all | ids/pids/ports, map on both, progress, default reached, 640x360, both closed, processes gone | ✅ 2026-11-11 (7/7) |
| I-13 | orphans: a stand-in game in which gmod-mcp never loads (`gmod-mcp-fakegame`), 3 clients with an owner label, 10 s timeout | timeout lists the 3 `silent_pids`; close by pid closes that one only and refuses a foreign pid; an unknown owner closes nothing; close by owner closes the other two; no process left | ✅ 2026-11-13 (6/6, no game) |
| I-17 | orphans, continued (no game): the timeout gives `answered: []`; `instances_list` without a game is not an error, has no instance, a "no game instance" note and the 3 silent clients with their owner; `{owner: "fork-b"}` lists none; after closing, none listed | ✅ 2026-11-14 (5 new checks, red before: 3 failures; full bridge suite 75/75) |
| I-14 | Live: 4 clients launched at once (light), all with gmod-mcp | the 4 answer (before 0.3.2, a client could take another's load in progress for a crash and skip gmod-mcp) | ⏳ needs the game |
| I-15 | Live: crash during the first load (kill a client while its marker exists), then 3 launches | exactly one launch skips gmod-mcp (`last_crash.txt` written), the next ones load it | ⏳ needs the game |
| I-16 | Live: instances_list health fields on 2 clients | agent_version, uptime_s, fps, main_thread_stalled, lua_errors per instance | ⏳ needs the game |

## Native unit tests (no game)

`gmod-mcp-unittests` and `gmod-mcp-coretests`, run by the CI on Windows and Linux.

| ID | Check | Result |
|---|---|---|
| U-1 | Crash guard: no marker; another instance loading (live pid); crashed load (dead pid) skips once; pid reused by another process; unreadable stamp; legacy marker removed; garbage and own stale marker; 5 launches at once with one old crash: exactly one skips; real process table (this pid alive, absurd pid dead) | ✅ 2026-11-13, 21 checks, Windows and Linux (WSL) |
| U-2 | Lua path lookup: mcpagent_tmp first; `addons/x/lua` (file or folder, trailing slash, backslashes); include()-style names in lua/, addons/*/lua/, gamemodes/; absolute; folder refused where a file is needed; misses list every place tried; escapes refused; chunk name | ✅ 2026-11-13, Windows and Linux |
| U-3 | `fields` projection: keeps only the named keys of returned tables, leaves other values, errors and calls without fields untouched | ✅ 2026-11-13 |
| U-4 | LuaJIT dump reader: debug info excluded from bodies; function found in place, after lines added above, not found when it or a nested function is edited; same code twice prefers the loaded line; stripped dumps; malformed input (empty, foreign, unknown version, truncated, every prefix, oversized lengths) refused without reading past the end | ✅ 2026-11-13, 20 checks, dumps built from the format |
| U-5 | Live: `lua_function_source` on an addon function: unchanged file, line added above, function edited without reloading, then lua_reload; a lua_exec-defined function; a C function; client realm (copy sent by the server) | ⏳ needs the game (also checks that `string.dump` and `util.Base64Encode` exist in each realm) |
| U-6 | Live: `convar_set volume_sfx 0.25` from the client realm | the value takes, `via: engine` | ⏳ needs the game |
| U-7 | Live: `lua_check {path: "addons/<addon>/lua"}`, `lua_exec_file` and `lua_job` with an absolute path and `fields` | resolved, only the fields come back | ⏳ needs the game |
| U-8o | Lua overlay folder check (`overlay::normalize_root`): addon root, its lua/ folder, trailing separators, spaces accepted; no lua/, missing, a file, relative, empty, quote, `;`, line break refused; the tool refuses a bad path before touching the main thread; `-mcp_overlay "D:\dev\my addon wt"` read back from a command line | ✅ 2026-11-14, 17 checks (coretests) |
| U-9o | Bridge: `instances_launch {overlay}` with a stand-in game: a folder without lua/, a relative path, a quote, a missing folder are refused before anything starts; a lua/ folder with spaces gives `-mcp_overlay "<root>"` on the client's command line; the answer names the overlay | ✅ 2026-11-14, 7 checks (bridge case `overlay`) |
| O-1 | Live: worktree copy of a test addon (`lua/autorun/ovtest.lua` defining `OVTEST = "main"` installed under addons/, `"branch"` in the worktree); `lua_overlay {path}` then `lua_overlay {}` | `overlay` set, `mount_hook` true, the worktree's lua/ first in `search_paths.lsv` and `.lcl`; no crash with `empty_cache` | ✅ 2026-11-14 (fixture: an overlay folder outside the game, with an autorun only there and a copy of a lua/mcpagent_tmp/ file): set, `mount_hook` true, overlay first in lsv/lcl. **First run: Lua errors everywhere and a black sky**: the root was at the head of MOD, so the engine mounted the overlay's lua/ instead of garrysmod/lua. Fixed (MOD tail, no GAME), and `base_lua_mounted` now reports it |
| O-2 | Live: `map_load gm_construct` after O-1; `OVTEST` in server and client realms; `debug.getinfo` / `lua_function_source` of a function from the overlay | `"branch"` in both realms; source = the worktree's file (check what `short_src` shows) | ✅ 2026-11-14 after the fix: map loads, gamemode loaded, no Lua error; the overlay's autorun ran in server and client realms |
| O-3 | Live: a file only in the worktree, `include()`d by the addon's autorun; a file only in the addon | both found (overlay first, addon as fallback) | ✅ 2026-11-14: `include()` of a file present in both lua/ and the overlay returns the overlay's; files only in the game still load (gamemode, PBA addon) |
| O-4 | Live: a second `map_load` | still `"branch"` (the MountLua hook re-adds the overlay; `hook_calls` grows, `hook_path_ids` shows lsv and lcl) | ✅ 2026-11-14: `hook_calls` 2 (`lsv` 1, `lcl` 1) after one map load; garrysmod/lua still mounted (`base_lua_mounted` true) |
| O-5 | Live: `lua_overlay {path: null}`, `map_load` | `"main"` again, `removed` all true, worktree gone from `search_paths` | ✅ 2026-11-14: `removed` all true, overlay gone from both lists; after map_load the game's copy runs again and the overlay's autorun is gone |
| O-6 | Live: two clients via `instances_launch {overlay}` with two different worktrees | each sees its own branch; the user's game (if running) and a third client without overlay see `"main"` | ⏳ needs the game |
| O-7 | Live: client copy through `AddCSLuaFile` (a `cl_` file only edited in the worktree), singleplayer | the client realm runs the worktree's version | ✅ 2026-11-14: a file sent with `AddCSLuaFile` from the overlay's autorun runs its overlay version on the client (singleplayer) |
| O-8 | Live: same file name and same mtime in addon and worktree (touch both), with and without `empty_cache` | which copy runs; documents the cache risk | ⏳ needs the game |
| U-8 | Lua runtime without the game (`luajit tests/lua/runtime_test.lua src/lua`): plain values; 200 KB string, 10-deep table, 10 000-entry table returned whole; explicit max_string/depth/limit cut and flag `truncated`; 3 M values fail with `result_too_large` early; cycles; `chunkname`; explicit budget stops a top-level infinite loop and a loop over compiled code; functions defined by lua_exec (with or without budget) and by a job run as fast as plain code; code timed inside lua_exec runs compiled; `jit: true` accepted; old `run_budget` signature; job `args` global, generation, `fresh_env` writes stay local but reads see _G, values and reports whole, explicit max_string flagged, runaway job stopped by explicit resume_limit_s; concurrent job reported, `job_list`, cancel of a sleeping job, async result kept past 300 s | ✅ 2026-11-14, 33 checks (red on the previous runtime: 16 failures) |
| U-9 | Live: `lua_exec` returns a 150 KB string, a 10-deep table, `_G` | whole, whole, `result_too_large` within seconds, game responsive | ✅ 2026-11-14: 150 KB string and 10-deep table whole; a client `_G` came back whole (4.5 MB of JSON, game responsive; Claude Code saved it to a file): the expectation was wrong, `result_too_large` is for > 1 M values or 64 MiB (3 M values: error, checked in the retex case) |
| U-10 | Live: a Think hook defined by `lua_exec`, timed from a timer, vs the same hook defined from a file (`lua_exec_file`) and vs `jit: true` (before 0.3.2: 44 ms vs 16.7 ms) | same timings | ✅ 2026-11-14 (server realm): a function defined by lua_exec 39.7 ms, the same compiled by CompileString 49.3 ms, with jit.off(f, true) (the old behaviour) 118.9 ms |
| U-11 | Live: `lua_exec "while true do end"` without budget, `timeout_s: 5` | `main_thread_timeout` after ≈ 5 s, `status` shows `main_thread_stalled`, `game_terminate` works | ✅ 2026-11-14 (60 s default wait): `main_thread_timeout` after 60 s, `status` main_thread_stalled 63 s, `game_terminate` ended it |
| U-12 | Live: `lua_job {async: true}` on server + another on client at once; `lua_job_status` with and without id, `wait_s`; `lua_job_cancel`; a second job in the same realm gets `concurrent_jobs`; map change during an async job | both run side by side; status shows progress then the result once; cancel stops it; `not_found` after a map change | ✅ 2026-11-14 (test_bridge.py retex): async id at once, concurrent_jobs, status with and without id, wait_s returns the result, collected job forgotten, cancel ends as cancelled (one realm; map change during a job not run) |
| U-13 | Live: `lua_job {fresh_env: true}` writes `X = 1`; next job reads `X` | nil (and `X` absent from `_G`); without fresh_env: 1 | ✅ 2026-11-14 (retex case): fresh_env global absent from _G; without fresh_env it lands in _G |
| U-14 | Live: `EyePos()` inside a job on the client vs `LocalPlayer():EyePos()` | the wiki only says "only reliable inside rendering hooks"; check what a job actually gets (agents saw 0,0,0) | ⏳ needs the game |
| U-15 | Console helpers (coretests): `a 1;b 1` -> 2 statements; blanks and empty statements dropped; `;` inside quotes kept; line breaks split; unclosed quote; command name lower-cased, quoted name; `Unknown command "x"` lines parsed (other wordings ignored); `[mcpagent] ` own log lines recognised only at line start | ✅ 2026-11-14, 16 checks (red before: 10 failures) |
| U-16 | `console_wait` (coretests, lines pushed from a thread with noise): 3 matches returned with next_cursor after the third; a second wait from that cursor gets the next 2, not the first ones; without cursor it starts now and times out with an error in ≈ 0.3 s; bad regex -> invalid_params | ✅ 2026-11-14, 8 checks |
| U-17 | Error map-load tags (coretests): errors of two map loads carry `map` and consecutive `map_load`; `since_map_load` keeps only the current one; result gives the current `map_load` | ✅ 2026-11-14, 6 checks (red before: 4 failures) |
| U-18 | Small helpers (coretests): `status`, `wiki_search`, `wiki_page`, `map_list` callable without instance, `lua_exec`, `console_exec`, `ents_find`, `api_search`, `lua_check`, `convar_get` not; `status {fields}` keeps the named keys (missing ones absent) and leaves errors and calls without fields alone; dead-instance message names id and pid, says "exited" + instances_launch, or "still running" + game_terminate | ✅ 2026-11-14, 13 checks (red before: 9 failures) |
| U-19 | Live: `console_exec "sv_gravity 300;sv_friction 3"` on the server | both set; `statements` has 2 entries; `convars` reads back 300 and 3 | ✅ 2026-11-14: `sv_gravity 500;sv_friction 7` both set, `statements` 2, `convars` read back 500 and 7 |
| U-20 | Live: `console_exec "pba_typo 1"` | error `unknown_command` naming `pba_typo` (check the engine's exact wording) | ✅ 2026-11-14 after a fix: GMod prints `Unknown command: name` (menu, client, server realms), not the quoted Source form; first run answered ok, now `unknown_command` |
| U-21 | Live: `console_exec` on an archived convar (e.g. `cl_drawhud 1`) and on a plain one | `convars` value and number; archive warning only on the archived one | ✅ 2026-11-14 (retex case): naming `fov_desired` reads it back with the ARCHIVE warning (read only); plain convars without warning |
| U-22 | Live: a `lua_job` printing `STEP n` every 0.2 s, `console_wait {regex: "^STEP", count: 5}` | returns after ≈ 1 s with 5 lines; progress notifications | ✅ 2026-11-14: a timer printing `BATCH DONE n` every 0.5 s, `console_wait {count: 4}` returned the 4 lines after 2.1 s, run as an MCP task |
| U-23 | Live: raise a Lua error on gm_construct, `map_load gm_flatgrass`, raise another | `errors_since {cursor: 0}` lists both with their maps; `since_map_load: true` only the second | ✅ 2026-11-14: errors before and after a map reload both listed with `map` and `map_load` 1 / 2; since_map_load keeps the current one (retex case) |
| U-24 | Live: `status` on a light test client | `fps_max` 20, `host_timescale` 1; `status {fields: ["map", "fps_max"]}` returns only those | ✅ 2026-11-14: light client `fps_max` 20, `host_timescale` 1; `status {fields}` returns only those (retex case) |
| U-25 | Live: 2 instances, `map_list` without `instance` | answers (no `ambiguous_instance`) | ✅ 2026-11-14: `map_list` without instance with 2 clients answered |
| U-26 | Live: kill a test client (`game_terminate`), then call a tool with its `instance` | `instance_gone`, pid, "exited", instances_launch hint (and later the same after the member is purged) | ✅ 2026-11-14: after `game_terminate`, `status {instance}` -> `instance_gone`, pid, "its process has exited", instances_launch hint |
| U-27 | Live: 2 clients with owners `a` and `b`, `instances_list {owner: "a"}` | only `a`'s client, entries labelled with `owner` | ⏳ needs the game |
| U-28 | Live: `ents.FindByName` on the client vs the server for a named map entity | the wiki says "Doesn't do anything on client" and Entity:GetName/SetName are server-only: confirm the client gets nothing | ⏳ needs the game |
| U-29 | Live: `coroutine.running()` in code run from a `timer.Simple` (agent PO3 saw non-nil on the main thread) | the wiki says it returns nil outside a coroutine: check, then document | ⏳ needs the game |

## Linux module (dedicated server in WSL)

2026-11-11, WSL Ubuntu 22.04, Garry's Mod dedicated server (app 4020, `-beta x86-64`: the default branch has no 64-bit
server on Linux), `srcds_run_x64 ... +sv_lan 1 +map gm_construct`.

| ID | Check | Result |
|---|---|---|
| X-20 | First load of the Linux module | **crashed the server** (abort while unwinding a C++ exception in `gmod13_open`: the module's static unwinder called the game's `libstdc++` personality routine). The crash sentinel then skipped the module on the next start, as designed. Fixed: `-Wl,--exclude-libs,ALL -Wl,-Bsymbolic` keep the module's C++ runtime private; `gmod13_open` can no longer let an exception reach the game |
| X-21 | Load after the fix | ✅ `active: mode srcds ... http://127.0.0.1:27080/mcp` |
| X-22 | Bridge suite, live cases (Linux bridge, Linux module) | ✅ 17/17 |
| X-23 | Direct calls: status (srcds, linux64, map), server Lua, ents_find, console_exec capture, lua_check bytecode reader, syntax error line | ✅ 6/6 |
| X-24 | Official conformance suite, 2026-07-28 requirements (from Windows through WSL's localhost forwarding) | ✅ 120 passed, the same as on Windows (only the suite's fixture scenarios fail) |
| X-25 | 2026-11-14, Windows srcds (`D:\gmod-srcds`, `+ip 127.0.0.1 +sv_lan 1`), test_bridge.py `retex` + `b` | 27/30 first: Lua errors from chunks without a .lua name were not parsed from the console (fixed, coretests +4), echo is blocked from server Lua without an engine path on srcds (test uses convars), no menu realm (test skips it). After the fixes: ✅ 29/29 |
| X-26 | 2026-11-14, Windows srcds: `lua_overlay` | the filesystem interface is not in filesystem_stdio (not loaded) nor reachable through dedicated.dll's CreateInterface: the overlay now says it is client only |

## Official conformance suite

`@modelcontextprotocol/conformance@0.2.0-alpha.12 server --requirements 2026-07-28`, against a test client (`-multirun`,
no map) through a local relay that adds the bearer token (the suite cannot send one) and logs the traffic. Run on
2026-11-11 with the **old** module (before the fixes below): 103 checks passed, 73 failed. With the new module: 120
passed; server-stateless 24/28 (the 4 left need the suite's diagnostic tools), dns-rebinding 2/2, custom headers 10/10. A first run had 101 failures
caused by the relay itself (it sent two Host headers), not by the server.

| Area | Result with the old module | Status |
|---|---|---|
| Missing `_meta`, `protocolVersion` or `clientCapabilities` → -32602 + 400 | accepted | fixed, confirmed with the new DLL |
| `MCP-Protocol-Version` header ≠ `_meta` version → -32020 + 400 | -32022 (version checked first) | fixed, confirmed with the new DLL |
| `initialize` / `ping` in 2026-07-28 → 404 + -32601 | answered | fixed in the module and the bridge (bridge tested) |
| Local `Origin` (`http://localhost`) accepted | 403 for any Origin | fixed: local origins accepted, others 403; confirmed with the new DLL (dns-rebinding 2/2) |
| `Mcp-Param-Instance`: invalid Base64, non-alphabet characters, value in the body without the header → -32020 + 400 | accepted | fixed, confirmed with the new DLL |
| Discover, serverInfo, unsupported version, unknown method 404, subscriptions (ack, id, filter), caching hints, lists, completion, resource not found, SSE multiple streams, standard header validation | pass | — |
| tools-call-*, prompts-get-*, resources-read-*, input-required-*, json-schema-2020-12, logging and missing-capability checks | the suite's own fixture tools and resources (`test_simple_text`, `json_schema_2020_12_tool`...) do not exist here; the suite reports them "not found" / "untestable" | not applicable |
| Tasks extension (forced): `tasks/get`, `tasks/update`, `tasks/cancel` from a client that did not declare the extension → -32021 + 400 with `requiredCapabilities` | -32602 (task not found) | fixed, confirmed with the new DLL |
| Tasks extension: every other check | drives the suite's fixture tools (`slow_compute`, `greet`, `failing_job`, `confirm_delete`, `multi_input`...) | not applicable without fixtures |
| 2025-11-25 requirements: initialize, ping, lists, completion, session lifecycle, SSE streams | pass (37 checks) | — |
| 2025-11-25: `logging/setLevel`, legacy `resources/subscribe` / `unsubscribe` | -32601: not offered by design (no logging capability; legacy subscriptions would need a GET stream, the 2026-07-28 ones are offered) | by design |
| 2025-11-25: tools-call-*, prompts-get-*, resources-read-*, elicitation-*, json-schema-2020-12 | fixture tools and resources | not applicable |

Constants: `tests/protocol/check_constants.py` checks every error code and `_meta` key of `src/mcp_protocol.h` against the
official 2026-07-28 schema (pinned commit, SHA-256 verified); runs in CI. Mutation check: changing -32020 to -32021 fails.

## Cross-cutting

| ID | Procedure | Expected | Last result |
|---|---|---|---|
| X-01 | 10 consecutive `map_load` (alternating maps, maxplayers 1/2), a call in all 3 realms between each | 10/10 OK, no crash | ✅ 2026-11-07 |
| X-02 | request without token / with a web `Origin` | rejected (T-03, T-04) | ✅ 2026-11-07 |
| X-03 | launch without `-mcp` | "inactive" log, no port opened, game works | ✅ 2026-11-07 |
| X-04 | network lock: `console_exec connect 192.168.1.99:27015` (another LAN machine) | `forbidden` (not a server on this machine) | ✅ 2026-11-07 |
| X-05 | client joins the local srcds through this machine's LAN address (192.168.1.10:27015; GMod srcds ignores packets from 127.0.0.1) | client realm allowed, reason "server runs on this machine (… address of this machine's network interface 192.168.1.10)" | ✅ 2026-11-07 |
| X-06 | infinite loop → `game_terminate` → relaunch → `map_load` + `lua_exec` | everything works again | ✅ 2026-11-07 |
| X-07 | coordinator (client) + member (srcds): `instances_list`, relayed `lua_exec(server)` on srcds, net message srcds -> client | 2 instances; `relayed_from`; client `net.Receive` reports `hello from srcds` through `reports_read` | ✅ 2026-11-07 |
| X-08 | kill the coordinator (client) | srcds takes over the base port in ≈ 7 s (`role:"coordinator"`) | ✅ 2026-11-07 |
| X-09 | relaunch the client after re-election | next free port (27082), registration with srcds, relay to the client | ✅ 2026-11-07 |
| X-10 | kill a member / call an unknown instance | removed from `instances_list` in under 13 s / `unknown_instance` | ✅ 2026-11-07 |
| X-11 | srcds: `map_load` (changelevel) | module kept, runtime reinjected (generation 2) | ✅ 2026-11-07 |
| X-13 | `mcpagent_setup` in the client console / srcds console / run as a player on srcds | full `claude mcp add` command and `.mcp.json` snippet with the base port / same / nothing printed (token never sent to a player) | ✅ 2026-11-07 |
| X-12 | connection failure ("Connection failed", non-clickable native dialog) | `disconnect_reason` event with the reason | ✅ 2026-11-07 |

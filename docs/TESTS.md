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
| L-27 | `lua_check {path:"<folder>"}` on a folder of `.lua` files; `{code}` and `{path}` together; missing path | `file` = `lua/mcpagent_tmp/...` for each file; `invalid_params`; `not_found` | ⏳ needs a game restart with the new DLL |
| L-28 | `lua_exec "local x = = 1"` | `lua_syntax_error` with `details.line` 1 | ⏳ needs a game restart with the new DLL |
| L-29 | `lua_reload {path:"autorun/<file>.lua"}` for a shared addon file edited after the map loaded; then `cl_`, `server/` and an ambiguous name | runs in server then client, the new code is live on both; `cl_` → client only, `server/` → server only; ambiguous → first match + `other_matches` + `warning`; unknown → `not_found` | ⏳ needs the new DLL |
| L-30 | `lua_exec` loop benchmark with and without `jit: true`; a function defined by a `jit: true` chunk then timed from a timer | `jit: true` several times faster; no `budget_exceeded` possible; default keeps the budget | ⏳ needs the new DLL |
| L-31 | `lua_job {jit: true}` | runs, no resume limit, result returned | ⏳ needs the new DLL |
| L-32 | `console_exec {command: "echo hi"}` (wrong name) | `invalid_params` naming `command`, suggesting `cmd`, listing the accepted arguments | ⏳ needs the new DLL |
| L-33 | client started with `-multirun ... +map gm_construct` from a background script while another application keeps the focus (cursor parked at 40,40 eight times, read back after 150 ms) | the game does not pull the cursor back; console line `told it the focus was lost`; clicking the game still captures the mouse normally | red reproduced 2026-11-11 with the old DLL: `HasFocus` true, pulled back 8/8; sent by hand: `WM_ACTIVATEAPP(FALSE)` 7/8, `WM_ACTIVATE(WA_INACTIVE)` 0/8 and `HasFocus` false. ⏳ green needs the new DLL |

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
| B-21 | Live game tool list = embedded snapshot (same release) | same names and schemas | ⏳ needs the new DLL |
| B-15 | Module: 2026-07-28 request without `_meta` protocolVersion or `clientCapabilities` | -32602, HTTP 400 | ⏳ needs the new DLL |
| B-5 | Plugin assembled like CI (skill, `.mcp.json` with `${CLAUDE_PLUGIN_ROOT}/bin/gmod-mcp-bridge`, bridge, module), installed in an isolated Claude Code 2.1.322 | `claude plugin validate` passes; `claude mcp list`: `plugin:gmod-mcp:gmod ✔ Connected`, also with nothing on the port | ✅ 2026-11-11 (Windows) |
| B-6 | Marketplace `archive` source pointing at a GitHub release asset (redirected download) | downloaded and unpacked (refused only because that zip is not a plugin) | ✅ 2026-11-11 |
| B-7 | Command without `.exe` in `.mcp.json` on Windows | `fb` resolves to `fb.exe`: one `.mcp.json` for both platforms | ✅ 2026-11-11 |
| B-8 | Version mismatch: module in the game older than the bridge | instructions start with "Version mismatch ..." | ⏳ needs a module of another version in the game |
| B-9 | Linux: build of the bridge, `--module-dir`, case A | builds without warnings, 5/5 | ✅ 2026-11-11 (WSL Ubuntu 22.04, g++ 11; a full build at -j 16 leaves ≥ 6.8 GB free) |
| B-10 | Real update cycle: release N installed, release N+1 published, marketplace refreshed | Claude Code offers / applies the update; new bridge after restart | ⏳ needs two releases |

## Official conformance suite

`@modelcontextprotocol/conformance@0.2.0-alpha.12 server --requirements 2026-07-28`, against a test client (`-multirun`,
no map) through a local relay that adds the bearer token (the suite cannot send one) and logs the traffic. Run on
2026-11-11 with the **old** module (before the fixes below): 103 checks passed, 73 failed. A first run had 101 failures
caused by the relay itself (it sent two Host headers), not by the server.

| Area | Result with the old module | Status |
|---|---|---|
| Missing `_meta`, `protocolVersion` or `clientCapabilities` → -32602 + 400 | accepted | fixed (needs the new DLL) |
| `MCP-Protocol-Version` header ≠ `_meta` version → -32020 + 400 | -32022 (version checked first) | fixed (needs the new DLL) |
| `initialize` / `ping` in 2026-07-28 → 404 + -32601 | answered | fixed in the module and the bridge (bridge tested) |
| Local `Origin` (`http://localhost`) accepted | 403 for any Origin | fixed: local origins accepted, others 403 (needs the new DLL) |
| `Mcp-Param-Instance`: invalid Base64, non-alphabet characters, value in the body without the header → -32020 + 400 | accepted | fixed (needs the new DLL) |
| Discover, serverInfo, unsupported version, unknown method 404, subscriptions (ack, id, filter), caching hints, lists, completion, resource not found, SSE multiple streams, standard header validation | pass | — |
| tools-call-*, prompts-get-*, resources-read-*, input-required-*, json-schema-2020-12, logging and missing-capability checks | the suite's own fixture tools and resources (`test_simple_text`, `json_schema_2020_12_tool`...) do not exist here; the suite reports them "not found" / "untestable" | not applicable |
| Tasks extension (forced): `tasks/get`, `tasks/update`, `tasks/cancel` from a client that did not declare the extension → -32021 + 400 with `requiredCapabilities` | -32602 (task not found) | fixed (needs the new DLL) |
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

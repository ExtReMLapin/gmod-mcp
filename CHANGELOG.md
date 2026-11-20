# Changelog

## Unreleased

### Features

- **The fixed vtable slots and offsets are verified before they are used.** The module called `ILuaShared` slots 6, 9 and 17,
  `IVEngineClient` slots 7 and 72 and `INetChannelInfo` slot 1 by fixed index, and trusted two struct offsets of the Lua state, with
  only the `IFileSystem` layout read from the code (the 5 or 9 slot `IAppSystem`). A game update that moves a slot would have made it
  call another function. Each one is now checked once per process against the code behind it (`src/layout.*`: the shape of the
  function, never exact bytes, so a recompiled build still passes): `GetLuaInterface` is the plain read of the realm array, `MountLua`
  calls `MountLuaAdd` for `"lua"` and `"gamemodes"`, `MountLuaAdd` and `UnMountLua` tail-call the file system (and their slots must
  agree with the `IFileSystem` layout that was read), `EmptyCache` walks the file cache, `ClientCmd` hands its text to the command
  buffer, `GetNetChannelInfo` returns a global and the net channel's `GetAddress` formats the remote address (and returns short
  printable text). A slot that is not recognized turns off only the feature that needs it, with the reason: no realms (`realm_unavailable`),
  `lua_mount` refused (`interface_missing`, the failed slots named; `empty_cache` alone is refused when slot 17 is the one), the blocked
  console commands not sent, and the client lock **fails safe** (no address: the client realm stays locked). `status.layout_check`
  reports `ifilesystem`, `iluashared`, `ivengineclient`, `inetchannelinfo` and `lua_offsets` (`ok`, `state`: `verified`, `pending` until
  the first use, `refused`, `not_applicable` on Linux and on a dedicated server; `reason`; the per-slot verdicts and counters), and a
  refused interface is logged once at load. Tested against the real bytes of two builds of each engine library.
- **`ui_click` and `ui_key`: click and type into a test client's UI.** Some UI has no console command: a VGUI2 dialog such as
  "Disconnected by the server", a Derma panel, a button of the menu's HTML. `ui_click {x, y, button?, double?, wheel?, coords?,
  screenshot_size?, wait_frames?, screenshot_after?}` posts `WM_MOUSEMOVE`, then the button down and up (double: down, up,
  double-click, up) a frame apart, or one `WM_MOUSEWHEEL`; `ui_key {key?, text?, wait_frames?, screenshot_after?}` posts a key
  (named keys, f1..f12; enter, tab, backspace and escape as key down, `WM_CHAR`, key up; a letter, a digit or space as the one
  `WM_CHAR` of the character, like `text`: the game's message loop turns a posted key down into its own `WM_CHAR` from the real
  keyboard layout, so key down plus `WM_CHAR` typed "aa" and "5(" on AZERTY) and/or `text` as `WM_CHAR` (256 characters at most). The engine narrows
  every typed `WM_CHAR` through the machine's ANSI code page with best-fit substitutions (live, CP1252: an accent of western Europe
  arrives, `ł` becomes `l`, `α` `a`, Cyrillic and CJK `?`; `WM_UNICHAR`, IME characters and `VK_PACKET` are ignored, so there is no
  other way in): a text with such a character is refused with `text_not_representable` (before anything is typed; `details`
  `code_page` and `characters`) unless `allow_lossy: true`, and the message points to `lua_exec` in the realm that owns the panel
  (`DTextEntry:SetText`; `SetValue` does nothing while the entry has the keyboard focus). The clipboard is never used. Coordinates are pixels of the `screenshot` image by default (`screenshot_size` converts from
  an image of another size, `coords: "window"` gives client-area pixels); the title bar is never part of them, and a client
  area that differs from the backbuffer the engine renders (DPI scaling) is scaled. The messages go to the game's own main window with
  `PostMessage`: the user's real cursor does not move and no window takes the focus (never `SetCursorPos`, `SendInput` or
  `SetForegroundWindow`). Each refusal has its own code: `unsupported` (srcds, not Windows), `not_a_test_client` (only games
  started by `instances_launch` accept input; `allow_user_game: true` overrides it), `client_locked`, `loading`,
  `window_unavailable` (minimized or not found), `cursor_hidden` (the game has the mouse: no menu, dialog or popup is open) and
  `out_of_bounds`; `cursor_hidden` and `loading` carry a `next_call`. The result lists what was sent, the point in client pixels,
  the window size, DPI and scale, and with `screenshot_after` the image like `screenshot`.
- **`screenshot` works at the main menu, in a dialog and on a loading screen.** Without a loaded map there is no client realm, so
  the engine's render capture had nothing to run in (`realm_unavailable`), and a VGUI2 dialog such as "Disconnected by the
  server" could not be seen at all. `screenshot` now uses the engine's own `jpeg` console command when no client realm exists,
  and keeps the render capture when one does. (Windows Graphics Capture was tried first: it returns an all-white image for this
  game's window, covered or not, so it is not used.) The new `source` argument (`auto` by default, `render`, `engine_jpeg`) forces
  one, and the result says which one it used (`source`). The module sends `jpeg` through the engine path `console_exec` uses for
  commands blocked from Lua, waits (4 s at most, `capture_timeout`) for the console line the engine prints,
  `Wrote 'screenshots/<name>.jpg': ... (WxH) ...`, taken after a console cursor read just before sending, reads exactly that file
  under `garrysmod/` and deletes it, and nothing else: the path must be `screenshots/<one name>.jpg` (no `..`, no other folder,
  no link), the file must not be older than the command, and a file that cannot be read stays where it is and is named in a
  `capture_failed` error; one that cannot be deleted is returned all the same with a `warning` that names its path. The result
  has `file` and `file_deleted`. It is the engine's own capture, so it works while other windows cover the game and takes no
  focus. The image is a JPEG at the backbuffer's size (the space `ui_click` reads), at the engine's own quality (the `jpeg_quality`
  convar, which is archived and left alone): `format`, `w`, `h` and `quality` are not applied there and are listed in `ignored`.
  One capture at a time. `ui_click` and `ui_key` `screenshot_after` use the same fallback (they used to answer
  `screenshot_note`). `screenshot_from` still needs a map.
- **`lua_job {wait: false}` is suggested as `async`** (issue #11). The unknown-argument message answered "did you mean
  'jit'?" for `wait` (the closest spelling). A small synonym table (`wait`, `await`, `sync`, `blocking`, `nowait`,
  `background`, `detach`...) now points to `async` when the tool has it. `wait` and `async` are opposites, so the message says
  so and the corrected `next_call` inverts a boolean (`wait: false` becomes `async: true`); a `wait` that is not a boolean gets
  the suggestion without a corrected call.
- **Long waits accept up to an hour** (issue #11). `wait_for`, `wait_frames`, `events_wait`, `reports_read {wait_s}` and
  `lua_job_status {wait_s}` took at most 600 s (`console_wait` already took 3600), so a long series needed chained calls. They
  now take 3600 s. The coordinator's relay of a call to another instance waited at most 900 s for the answer and would have cut
  a long wait on a test client at 15 minutes: it now waits 3700 s. The MCP client's own tool-call timeout is not ours to change
  (Claude Code: `MCP_TOOL_TIMEOUT`).
- **`lua_exec` and `lua_job` report a `RunConsoleCommand` the engine blocked** (issue #11). The engine ignores
  `RunConsoleCommand("fps_max", ...)` (and the other commands on its Lua blacklist) without a word, so an agent believed it had
  changed the frame cap. While the code runs, the runtime now notes the calls that `IsConCommandBlocked` refuses; the result
  carries `blocked_console_commands` and a `warning` pointing at `console_exec` / `convar_set`. Only calls made by the code
  itself while it runs are seen (a job: during each of its resumes); a command refused for another reason is not.
- **`fields` says when it projected nothing** (issue #11). On `lua_exec`, `lua_exec_file`, `lua_job` and `lua_job_status`,
  `fields` only keeps keys of returned tables; when the value was a JSON string (`util.TableToJSON`), a number or an
  array, the whole result came back with no word. The result now carries `fields_note` saying so and what to return instead.
- **`errors_since {group_by: "message"}`** (issue #11). One entry per distinct message (`message`, `count`, `first_seq`,
  `first_time`, `last_seq`, `last_time`, `realms`, `map`, `stack`, `addon`), most frequent first, instead of up to 2000
  mostly identical errors; `limit` caps the groups (`groups_total` counts them) and `next_cursor` covers every error read.
  The default output is unchanged.
- **The mounted Lua folder tells which copy ran, what is out of date, and reloads on demand** (issue #9).
  `lua_function_source` and `lua_inspect` (on a function) return `from_mount`, the real `disk` path, `mount_has_file`
  and `provenance` (`mount_in_place_at_load`, `reloaded_from_mount`, `installed`, `not_on_disk`, `no_file`): the engine names a
  mounted file `@lua/x.lua`, the same as the installed copy, so the name alone could not tell. The module records, when the
  hook puts the folder in front of a realm's Lua path, the folder's files and their times, and uses that, not the name.
  `status.lua_mount` lists `stale_files` (files whose time differs from what each realm loaded: modified, added, deleted; an
  older time after a `git checkout` counts) with the realms each runs in, and `realms_without_mount`; `map_load` empties the
  engine's Lua file cache first when a folder is mounted and warns when the check finds something. `lua_reload
  {mounted_changed: true}` runs every changed file again in its realms and reports them per realm; entity, weapon and
  effect files and deleted files are listed in `skipped`. A reload re-runs the file: it does not undo what the old copy did.

- **`instances_launch` passes frame caps, startup commands and extra engine options to the clients it starts** (issue #10).
  `fps_max` and `fps_max_nofocus` (0 = no cap, or 10 to 1000) replace the preset's frame cap, so a test that needs 45 or
  300 fps no longer has to change the archived `fps_max` with `console_exec` after the launch; the result reports
  `fps_max`, `fps_max_nofocus` and `fps_note`. `commands` runs console commands on each client once it answers, before
  its map loads (`instances[].commands` reports each one; a failure makes the call `isError`). `launch_args` adds engine
  command-line tokens (`["-noborder", "+cl_showfps", "1"]`). Everything is checked before a client starts: `quit`/`exit`,
  `connect`/`retry`/`redirect`/`connect_lobby`, map changes (use `map`), and the options gmod-mcp sets itself
  (`-multirun`, `-mcp*`, `-w`, `-h`, `-width`, `-height`, window-mode options, `-dxlevel`, `-condebug`, `-game`) are refused
  with `invalid_params` naming the offender, and a token with a space, quote, `;` or non-ASCII character is refused too
  (the bridge builds a command line). The archived convars these options change are given back to the user's value
  before a clean quit like the frame cap already was: the bridge lists their names to the module on the command line
  (`-mcp_restore_convars`) and the module restores those that are archived (a convar changed by `exec` of a cfg file is not
  tracked; a module older than this release restores only its four built-in convars). The tool description, the guide, the
  skill and the README say where the options are listed (Valve Developer Community "Command line options", the Garry's Mod
  wiki, `api_search`, `concommand_list`).
- **Hints are data as well as prose** (issue #3). Where a hint names the next call, the result also carries it as
  `next_call: {tool, arguments?, after_s?}` (inside `error` for an error), next to the unchanged prose: a missing realm
  (`map_load`), a frozen main thread (`status`), a crashed or ended game (`instances_launch`), a launch or map load refused
  while another game starts or loads (the same call, later, with a larger `timeout_s`), an argument typo (the corrected call),
  a started `lua_job` (`lua_job_status`), cut values (the call without the limits), a `lua_mount` a launched client lacks. In
  an offline eval (14 payloads, haiku and sonnet, 3 runs) the first call the model made was the right one 83-86 % of the time
  without it and 93-100 % with it, at about 22 more tokens per payload (`tests/eval/hint_eval.py`).
- **Every tool declares its `outputSchema`** (the shape of a success result; types, `required` only where every path sets the
  field, extra fields allowed). Clients that validate results (the MCP SDK in Claude Code) can now rely on it. Results are
  checked against it in the unit tests (`tests/schema/results.json`).
- The guide has a "Reading results" section: where hints live and what each hint field means.
- **Hung, crashed and memory-starved clients are reported** (issue #8). `instances_list` gives every instance `stalled_s` (seconds
  its main thread has been blocked) and `fps_low_s` (seconds under 10 fps, not counted while loading), and `state: "hung"`
  after 60 s of blocked main thread (180 s during a map load) with the advice to close and relaunch; an instance whose
  heartbeats stopped while its process lives stays listed (`unresponsive`, then `hung`) instead of vanishing after 10 s.
  A call in flight to an instance that dies answers `client_crashed` at once (the coordinator and, for the clients it
  launched, the bridge watch the process), and a call to a frozen process fails with `instance_hung` after 30 s without
  heartbeat. A client that died during its map load is `client_crashed` in `map_error`, not `game_not_running`. `instances_launch`
  checks the free commit and RAM before starting anything and refuses with `low_memory` (`free_commit_mb`, `free_ram_mb`,
  `running_clients`, `per_client_mb`) under 1.6 GB of commit and 1.2 GB of RAM per client (measured: a light client holds
  1.55 GB of commit); `ignore_low_memory: true` overrides it. A crash report carries `details.memory` and a hint when memory is low.

### Fixes

- **Code Mode (Claude Code's experimental mode where the model writes JavaScript): long calls are answered in place** (Issue #3).
  Claude Code marks the calls a script makes (`_meta["claudecode/toolUseId"]` starts with `repl_`). The game answered `map_load`
  and waits of 15 s or more with a background task to those calls too, and the script only got a "moved to the background" stub
  (`await map_load(...)` followed by more code broke); they are now answered in place. A new setting, the environment variable
  `GMOD_MCP_TASKS=off` (default `auto`), turns the tasks the server chooses off for every client: set it for Claude Code (the
  bridge then does not forward the Tasks extension to the game) or for the game (the module reads it too). A client that asks for a
  task itself (the 2025-11-25 form) still gets one.

- **Every error text is one JSON object, so `e.detail` works in Code Mode** (Issue #3). Claude Code throws the text of an error
  result and gives the script `e.detail` (the parsed JSON) only when that whole text starts with `{`. The version-mismatch warning
  was a prose block in front of an error without `structuredContent` (it is now a field `version_warning` inside the JSON), and two
  results of the bridge were `isError` with no `error` object: `instances_close` when nothing was closed (now `error.code:
  "refused"`) and `instances_launch` when the clients started but a map, a startup command or the `lua_mount` failed (now
  `error.code: "launch_incomplete"`; the whole result is still there). Checked on every error path the bridge tests drive.

- **A test client no longer keeps the user's foreground** (live, x86-64 branch). Every client `instances_launch` started became the foreground
  window and kept it, so the user's terminal lost the focus at each launch. The bridge now starts the client with a window shown without
  activation (`STARTF_USESHOWWINDOW`, `SW_SHOWNOACTIVATE`: three launches in a row and a launch of two, 0 of 1,216 samples taken during the
  launches differed from the window the user had) and records the foreground window before each start, passing it as
  `-mcp_return_focus <decimal window handle>`. If the client still ends up in front (a game started by hand with the option: the
  engine took it 0.4 s after the start), the module of a test client (`-mcp_test_client` only, never the user's own game) gives the foreground
  back with `SetForegroundWindow` once its window is the foreground one (allowed: the foreground process does it), at most 3 times and
  during the first 3 minutes, so a window the user focuses on purpose later is left alone; a target that is gone, hidden or minimized is
  not used. A later client of the same launch keeps the user's window as target instead of the earlier client's. The option's value is
  validated (plain decimal, 1 to 4294967295). Event `focus_returned`.
- **`ui_click` loses a click now and then while the game is the foreground window** (live, x86-64 branch). In front, the engine also
  reads the real cursor (nowhere near the posted point), and a click posted as one move then a press was lost about one time in five
  (11 of 60 on a test client in front; none in the background, 10 of 10). With the game in front `ui_click` now posts the move three times,
  a frame apart, before the press (0 of 90 lost). The real cursor is never moved (no `SetCursorPos`, no `SendInput`).
- **`instances_list` carries `silent_s` on every entry** (live verification of 0.3.7). A frozen member showed `state: "unresponsive"` and
  `stalled_s` but no `silent_s`, which only the `instance_hung` error had. Every entry now has `silent_s`, the seconds since its last
  heartbeat (0 for this game, rounded to 0.1 s, like the error's), and the `instances_list` result schema describes the entries
  (`state`, `heartbeat_age_s`, `silent_s`, `stalled_s`, `fps_low_s`, `loading`).
- **An unknown argument is named at the start of the `invalid_params` message** (live verification of 0.3.7). `console_exec {cmd,
  timeout_s}` was refused with a message that began with the validator's own words (`At  of {...} - validation failed...`), so a client
  that showed or cut the start of it never saw the name. The message now starts with `invalid arguments: Unknown argument '...'` (and
  `Missing required argument`), then the accepted list, the validator's words last; a wrong type keeps the validator's words first.
  `console_exec` has no wait budget (it waits `wait_frames` frames; `connect_timeout_s` is the wait of `connect` alone): `timeout_s`
  says so and points to `console_wait`, `wait_for` and `events_wait`, instead of suggesting `connect_timeout_s`.
- **Whatever the compiler answers that is not a function is a `lua_syntax_error`** (live verification of 0.3.7: `lua_exec` code with a
  `"\u0041"` escape, invalid in LuaJIT 2.1 which only knows `"\u{41}"`, came back as an empty result once). `A.compile` now
  returns a function or `nil` and a message in every case: a compiler that gives the message as its first value (as `CompileString`
  does) or nothing at all no longer reaches the code that runs the chunk. The plain case already answered `lua_syntax_error` with the
  message and line in the Lua runtime tests; the live empty result could not be reproduced outside the game.
- **`loading` is true while a `map_load` is waiting to start the load** (live verification of 0.3.7). `map_load` first waits for the
  menu to be idle (up to 15 s on a client whose loading screen is stuck) and 30 frames before the engine starts the load; `status` and
  `instances_list` said `loading: false` for those ~18 s, so a launch or another `map_load` was not held back. The call itself now
  counts as a load from its first moment to its answer, whatever the engine does (and a stuck loading screen is not reported as
  `loading_stuck` while it runs).
- **`map_load` with no game answers as fast as `status`**. It answered `game_not_running` after 2-3 s instead of 1 s: before the call itself,
  the bridge asked the directory which other games were loading (to report them), a second refused connection. It no longer asks
  once a call has found nothing listening.
- **A crash answer says where the dump's exception happened** (issue #8, live verification). `client_crashed` / `server_crashed` named
  the minidump and nothing else. The module now reads it (exception record, the faulting thread's stack pointer and stack memory, the
  module list) and `details` and the `instances_list` crash entry carry `fault` (e.g. `engine.dll+0x2069e0`) and `stack_candidates` (up
  to 8 stack words that point into a module, from the stack pointer up). The guide adds that a crash only on test clients may be
  timing (`fps_max` 20 runs several server ticks per frame): retry with `fps_max: 300` to tell.
- **`instances_launch` under a foreign-held base port** (issue #5, live verification). Every call already answered `port_conflict` when a
  program that is not gmod-mcp holds the base port, but `instances_launch` started its clients (which take the next free port, where the
  bridge never looks) and waited its whole budget to fail `timeout` "gmod-mcp did not load in them". It now refuses up front with
  `port_conflict` (the fix, `GMOD_MCP_PORT`, and the gmod-mcp found on the next port when there is one) and starts nothing; when the port
  is taken while a launch waits, it gives up after 10 s and the `timeout` error names the conflict.
- **A frozen but live instance keeps answering `instance_hung`** (issue #8, live chaos test). The first call to a member that stopped
  heartbeating (frozen as a whole) answered `instance_hung`, every later one `instance_gone` in 0.2 s although the process still
  ran: the member had been dropped from the directory and the tombstone assumed it was gone. `instance_gone` now means the process
  ended; a live silent one answers `instance_hung` (details `pid`, `silent_s`) with how to close it.
- **A stuck loading screen is not a load** (issue #8, live check I-C1). After `map gm_flatgrass` then `disconnect` within a second the
  engine kept `IsInLoading()` true for ever with nothing behind it, so `loading` stayed true in `instances_list` and `status` until the
  300 s cap and `instances_launch` failed `map_load_in_progress` for minutes. A loading screen with no server or client realm, no
  connection and no load starting for 12 s of polled time (a blocked main thread is not counted) no longer counts as a load: `loading` is
  false, `status` and `instances_list` say `loading_stuck: true` with the way out (a `map_load` clears it; `disconnect` does not, checked live). A real load,
  even a minute-long one with its realm present, is never cut.
- **`game_quit` on a dedicated server really quits (Linux) or says it cannot.** On srcds the console `quit` is blocked from server Lua
  (the engine's own blacklist), so `game_quit` answered `{quitting: true}` and the server kept running. The runtime now checks
  `IsConCommandBlocked("quit")`: when blocked, the module ends the process itself with SIGTERM half a second after the answer (the
  server's own clean shutdown, `via: "signal"`); where it cannot (Windows srcds) the answer is the error `blocked`, never
  `quitting: true` for nothing. `game_terminate` was already working on Linux (checked live).
- **The Linux module loads on the dev-branch server.** `plat::find_loaded_module` only tried `dlopen(bare name, RTLD_NOLOAD)`, but the
  dev branch loads `lua_shared_srv.so` by full path from `garrysmod/bin/linux64` (no SONAME, not on `LD_LIBRARY_PATH`), so the module
  never found the Lua library and stayed inactive. It now also walks the loaded objects (`dl_iterate_phdr`) for one with a matching
  file name and takes a handle on it by its full path.
- **`lua_mount` file times on libstdc++ < 13** (Linux, GCC 11/12). The file system clock of those libraries counts from the year 2174, so every
  current file time is a negative tick count, and `unix_seconds_of` took negative as unknown: the engine-serves probes then expected no
  time and a copy with another time was reported as served. Unknown is now an explicit marker (`kUnknownTime`), not a sign.
- **Crash on killserver**: `console_exec "killserver"` on a game that hosts a map (default realm or `realm:"server"`) crashed
  the game (access violation in engine.dll) whenever a frame ran several server ticks, as test clients at fps_max 20 always
  do. The engine disconnects the local client and switches to single-player at once but shuts the server down only later;
  the next tick of the same frame sends a snapshot from the half-torn server. `console_exec` now leaves the map first (as
  `disconnect` does, `left_game_first: true` in the result) and then runs `killserver`, which only resets the network mode.
- **A success result never fails an outputSchema because of a non-finite number** (issue #3, live fuzz). `trace` and
  `trace_through` with a huge end point, `players_list` / `player_set` after an overflowing or NaN eye angle, `ents_find` with a
  NaN origin and `convar_get` / `convar_set` with "1e999" answered `null` where the schema said `number`, which the MCP SDK in
  Claude Code rejects as a whole. Every number of every outputSchema now accepts `null` (a test walks all schemas), and the guide
  says a non-finite number reads `null`.
- **`instance_select` refuses an unknown id** (`unknown_instance`, with the known ids; the default stays). When the list of
  instances cannot be read the id is accepted with a warning.
- **`ents_find` explains id -1** (clientside-only entities have no index: no tool can address them) in a `note`.
- **`wiki_page` / `wiki_search` try the client and server realms** when the menu realm has no HTTP function, and say which
  realms were tried when none has.
- **`debug_draw` accepts `{type: "arrow", from, to, head?}`**.
- **Health reporting found wrong by live chaos tests** (issue #8). (1) A client frozen as a whole kept the figures of its last
  heartbeat in `instances_list` (`fps 19`, main thread ticking, `last_tick_age_s 0.05`) for the whole freeze: while the heartbeat
  is stale the entry reads `fps 0`, `main_thread_stalled true` and `last_tick_age_s` as old as the silence, and the last raw
  figures stay under `last_heartbeat`. (2) `map x` then `disconnect` at once left `loading: true` for minutes (every
  `instances_launch` then failed with `map_load_in_progress`) while `status` said false: the server Lua state had opened and
  closed between two pump runs, so nothing switched the load flag off. The pump now drops the flag once no server state
  exists, the server source alone counts 30 s at most once the loading screen is down (it was 300), and `status.loading` is the
  same flag as the list. (3) `instances_launch` refuses with `peer_unhealthy` while a running client is `unresponsive` or `hung`
  (a frozen client keeps a new one from registering, so the launch timed out with `0 of 1 clients answered`), unless
  `ignore_unhealthy: true`, and a timeout names the stuck peers. (4) With `count > 1`, a client that dies at startup no longer
  leaves its siblings unmentioned: the error lists those still running and warns they are not closed. (5) The `owner` label
  now travels with the game (`-mcp_owner`, heartbeat): `instances_list` shows it on every instance and
  `instances_close {owner}` closes `keep: true` test clients of an earlier bridge session (never a game that is not a test
  client, never one without the tag). (6) `low_memory` counts the game's processes, not only registered instances, and reports
  `registered_clients` and `game_processes`.
- **`lua_mount` no longer reports a mount that loads nothing** (issue #9, found by live chaos testing). The module said `active`
  and `loaded_with_mount` from its own bookkeeping for folders the engine takes into its search paths but cannot read. UNC
  paths (also `\\localhost\D$\...`), extended-length paths (`\\?\D:\...`) and roots whose files pass MAX_PATH are now refused
  with the reason, and with a map loaded each realm's file system is asked for a few of the folder's files:
  `engine_serves_folder` / `engine_serves` in `lua_mount {}`, `status.lua_mount` and `map_load` (a warning when false), and
  `instances_launch {lua_mount}` reports such a client `active: false` with `error_code: "mount_not_effective"`.
  A folder renamed or deleted (or only its `lua/`) is reported (`folder_missing`, `mount_folder_missing`) instead of an empty
  "all current" state, and `lua_reload {mounted_changed}` answers `mount_folder_missing`. A folder that replaces the game's own
  base Lua (`lua/includes/init.lua`: the client hung and `map_load` timed out after 180 s) is refused with `shadows_base_lua`
  and the files listed, unless `allow_shadow: true`; a `map_load` timeout with a mount set names the mount. After the mounted
  folder changes without a map load, a function that ran from the old folder reports `provenance: "mount_changed_since_load"`
  with its old copy as `disk`; a file loaded in place and then reloaded is `reloaded_from_mount`. A 30000-file mount no longer
  costs seconds per `status`, `lua_reload {mounted_changed}` or `map_load` (one cheap scan instead of one slow scan per
  realm: 5000 files 1064 ms -> 16 ms). The warning for a realm without a load record no longer claims it ran the installed
  files. Smaller: stored paths compare like Windows does (trailing dot, case), a junction into the game folder is refused,
  unmount removes the `gamemodes` paths even when that folder is gone, and a failing file in
  `lua_reload {mounted_changed}` is an error result (`reload_failed`, details hold the whole result).
- **`lua_mount` works again on the x86-64 build of Garry's Mod of 2026-11-19** (issue #9). That build added four slots to
  the engine's `IAppSystem` interface, so every `IFileSystem` slot moved by four: the module's `AddSearchPath` and
  `RemoveSearchPath` calls landed on two functions that do nothing, and the mounted folder was silently never searched
  (the hook ran, the folder's autorun did not). The old `GetSearchPath` slot became `RemoveAllSearchPaths`: the one call
  the module made wiped every search path, and the next load of a new model blocked the game for good (that call was
  already removed). The module now reads the layout from the engine's code and refuses the mount, with a message, when it
  matches no known build; `lua_mount` reports `fs_first_slot` (5 on older builds, 9 now).
- **`fps` in `instances_list` and `status` is counted over the last 3 s** (issue #8, found with issue #11 item 2): it was the
  mean of the last 256 frames, so a client that ran at 2 fps through a map load kept reporting about 2 fps for minutes
  after it ran at 20 again, and read the rate from before while its main thread was blocked. It reads 0 when blocked.
- **The current x86-64 install of Garry's Mod is found** (issue #4). It has no `gmod_win64.exe`: the 64-bit game is
  `bin\win64\gmod.exe`, and the `gmod.exe` at the root of the game folder is the 32-bit one. `instances_launch` now looks
  for `gmod_win64.exe` at the root, then for `bin\win64\gmod.exe`, prefers the file whose PE header says x86-64, never
  starts a 32-bit one, and starts the game with the game folder as working directory. The `game_not_found` message names
  both paths. The skill no longer tells an agent to give up when `gmod_win64.exe` is missing: it gives the
  `bin\win64\gmod.exe` start command and how to tell a 32-bit executable from a 64-bit one.
- **A foreign program on the base port is no longer reported as a stopped game or a token problem** (issue #5). When
  another program held port 27080, the tools failed with `the game answered HTTP 403 without a JSON-RPC response`, the
  server instructions said the game was not running, the game's log blamed the token and the skill took any HTTP answer
  for a running game. Now the module signs every HTTP answer with an `X-Gmod-Mcp` header (modules before this version are
  recognised by the JSON body of their 401). The bridge answers `port_conflict` (tool errors, server instructions,
  `instances_list` note) when the base port answers without that signature: it names the port, how to find the program
  (`Get-NetTCPConnection -LocalPort`, `ss -ltnp`) and `GMOD_MCP_PORT`, and looks in the background for a gmod-mcp on the
  next 15 ports to name the value to set. A gmod-mcp 401 stays "rejected the token". In the module, a coordinator link
  to a foreign program is `foreign` in `status` (`coordinator_link`) with a log line saying the port is held by another
  program; `refused` (token) is only for a real gmod-mcp coordinator. The skill checks for `401` plus the header, and
  explains how to find and handle the program.
- **Map loads the bridge does not see no longer crash a game that is being started** (issue #7, Garry's Mod issue 7137).
  The launch/`map_load` lock only knew the calls that went through a bridge: a map loaded from the console (`map`,
  `changelevel`), from Lua (`RunConsoleCommand`) or by a script talking HTTP straight to a game was not serialised against
  `instances_launch`, and a launch starting while it ended could crash it. Every game now reports `loading` (a map load in
  progress, whatever started it) with its heartbeat, sent at once when a load starts or ends, and `instances_list` shows it.
  `instances_launch` waits while any game loads (`waited_s`, `waited_reason` starting with `map_load_outside_bridge`; at the
  call's `timeout_s` it fails with `map_load_in_progress` and starts nothing). `map_load` does not wait for such loads but
  lists the other games that were loading when it started (`other_games_loading`). Still not covered: a game you start by
  hand, a game without gmod-mcp, and a console or Lua map load started while a launch is creating its processes (the bridge
  only delays its own calls). Everything is tagged `FP7137` and obeys `GMOD_MCP_FP7137_WORKAROUND=0` (see
  `docs/FP7137.md`); the agent-facing texts (server instructions, `instances_launch` / `map_load` descriptions, guide,
  skill, README) say so.
- **A call lost in flight while the coordinator moves is `relay_lost`, not `game_not_running`** (issue #6, live chaos test).
  When the coordinator died with a call in flight and other games were running, the call answered `game_not_running`
  ("Start the game with -mcp") although its target game lived and the action had happened (a map load completed, a Lua job
  kept running). It now answers the retryable `relay_lost` (`details.retry_after_s`, `details.maybe_executed: true`): the call
  may have run, check the state before repeating one that is not idempotent. `instances_launch {map}` asks the client for its
  `status` after a lost `map_load` answer and reports the map loaded (`map_note`) when it sits on it, instead of `map: null`.
- **A call made in the first moments after the coordinator died is ridden out** (issue #6, live chaos test). The dying
  coordinator's listener still accepts a connection for a fraction of a second, then drops it; only a refused connect counted
  as "coordinator gone", so such a call (and the first calls of bridges that had launched no client) answered
  `game_not_running`. A connection that breaks within 0.5 s with nothing of an answer now counts as refused: with other
  games known to live the bridge waits for the next coordinator like for any hand-over.
- **No wait for a hand-over that cannot happen** (issue #6, live chaos test). After the last games closed, other bridges
  answered `coordinator_changing` for up to 30 s (a call waited 10 s) because "several games live" stayed in their memory for
  30 s. The bridge now remembers which processes could take the coordinator role over (the members of the last list that
  showed several games) and answers `game_not_running` at once when none lives any more, or when a later list shows a single
  game.
- **A game killed together with the coordinator is reported as gone, not `unknown_instance`** (issue #6, live chaos test).
  The new coordinator had never heard from a member that died with the old one: calls for it blocked 7-8 s on
  `coordinator_changing`, then answered `unknown_instance`. Heartbeat answers now carry the directory of instances (the
  coordinator included); the member that takes the base port over keeps a tombstone for every instance it does not hear from,
  and a call for one whose process ended answers `instance_gone` / `client_crashed` at once, with the same reasons as always
  (exit code, dump). A crashed old coordinator is now listed as `crashed` like any other crashed instance.
- **At most 2 maps load at a time across every bridge** (issue #7, live chaos test). The limit of 2 was per call: two
  `instances_launch {count: 2, map}` plus a `map_load` had 5 games loading at once. Every map load of every bridge now
  registers in the shared FP7137 state and waits while 2 others are registered (`map_load_in_progress` at the timeout; the
  kill switch lifts it).
- **A killed bridge no longer leaves its `keep: false` clients running** (issue #8, live chaos test). `taskkill /F` on a
  bridge ran no exit code, so the test clients it had launched ran for ever. On Windows they are now started in a job object
  that closes with the bridge (`KILL_ON_JOB_CLOSE`), helper processes of the game included; `keep: true` clients stay out of it.
- **`low_memory` counts games, not their helper processes** (issue #8, live chaos test). The `game_processes` figure counted
  every process running the game's executable; each game starts helper processes of the same executable (`--type=renderer`,
  `--type=utility`), so it was about 3 times too high and could refuse a launch the machine could take. A process whose parent
  is a game process that started before it is a helper and is no longer counted.
- **`instances_close` says `already_gone` for a process that had already ended** (issue #8, live chaos test). `{owner}`
  listed a pid that had quit before the call as `closed`. Such pids (a client of this session that quit by itself, another
  session's test client whose process is gone) are now in `already_gone`, with a note, and nothing is sent to them.
- **The instance directory survives the coordinator's exit** (issue #6). The game that holds the base port keeps the
  directory of instances; when it ended, `instances_list` was empty and every call with `instance` failed with
  `unknown_instance` for about 10 s (a refused loopback connection takes 2 s on Windows, and the members waited for
  three failed heartbeats). Members now try to take the base port over at their first failed heartbeat (the bind only
  succeeds when nobody holds it), the new coordinator asks every other port of the range to re-register at once, and
  for 8 s it answers the retryable `coordinator_changing` (with `retry_after_s`) instead of `unknown_instance`
  for an instance that has not registered yet. The directory is complete again about 3 s after the coordinator ended
  (about 10 s before). The bridge waits for the new coordinator (up to 10 s) and retries `coordinator_changing`, but
  only when other instances are known to live: with the only game closed a call still says `game_not_running` at once.
- **`instances_launch {lua_mount}` never leaves a client without the mount.** Each client is asked what its engine has
  BEFORE any map loads (it was after, so a client without the mount had already loaded the map), a missing mount is applied
  again with `lua_mount {path}`, and a client that still has none gets no map (`map_error` `lua_mount_not_active`, isError).
  An answer lost to a coordinator change while checking is waited out. A launch that times out says its silent clients keep
  running and must be checked with `lua_mount {instance}` before any `map_load`.
- **Closing the coordinator says so.** `instances_list` names it (`coordinator_id`, `role`) and warns when it is a client
  of this session (`coordinator_warning`); `instances_close` reports `was_coordinator` and `coordinator_moved_to`;
  `game_quit` / `game_terminate` on the coordinator add `coordinator_moved`, `remaining_instances` and a warning.
- **A call with the game not running answers in the module's error shape.** `game_not_running` is now
  `{"error": {code, message, details: {host, port, reason}}}` as the only text block and as `structuredContent` (it was prose
  next to the JSON). The version-mismatch warning is a field of the result's JSON (first key of the text) instead of a prose
  block in front of it, so the text stays the JSON of `structuredContent` (what code-mode hosts parse).

## 0.3.6

Guards against an engine crash when a game starts while another finishes a map load (Garry's Mod issue 7137),
reports crashed games as such, keeps test clients out of the user's video settings and config, and fixes the install
steps found by a literal walk-through of the README.

### Features

- **A game that crashed is no longer taken for one that quit** (point 3 of issue #2). A call to an instance whose process
  ended without a clean shutdown answers `client_crashed` (`server_crashed` for a dedicated server) instead of a timeout,
  `instance_unreachable` or `instance_gone`, with `details.dump` (the minidump file name in the game folder's `crashes/`,
  null when none was written), `details.exit_code` and `details.evidence`. `instances_list` keeps a crashed instance for an
  hour as `state: "crashed"` with a `crash` object (live ones have `state: "running"`), a `map_load` in flight when the
  client died reports the crash, and `instances_launch` reports a client that crashed at startup or during its map load
  (`map_error`). A harness can relaunch with `instances_launch`; the dump is for a human.
  How it decides: a game that shuts down cleanly says goodbye to the coordinator, and `game_quit` / `game_terminate` sent
  through it count as asked to end; otherwise a dump written in the 20 s before the process ended (matched by time, never
  given to two instances) means crashed, then the exit code read through a handle the coordinator holds since the instance
  registered (0 = quit, 0xC0000005 and other 0xC... = crash). Limit: a process killed from outside with no dump
  (exit code 1) looks like a crash with `dump: null`; when several games crash together, dumps are matched by time only.
  Dead instances are now noticed when their process ends, not 10 s later. Tool descriptions, the guide, the skill and the
  README say what the codes mean.

### Fixes

- **`mcpagent_uninstall` did not always restore the original `menu.lua` byte for byte.** When the original had no final
  line break (also an empty file, or CRLF without a final line break), the installer added one and removal could not
  tell. The installer now always adds one line break, the block and one line break, and removal takes exactly those
  away; a file installed by an older version is restored from the backup. A test covers LF, CRLF, mixed endings and
  files with and without a final line break.
- `mcpagent_setup` prints `claude mcp add --scope user ...`: without a scope the entry only exists in the folder where
  the command ran.

- **Test clients no longer leave their frame cap in the user's config.** `instances_launch` starts its clients with
  `+fps_max 20 +fps_max_nofocus 20 +snd_mute_losefocus 0`. These convars are FCVAR_ARCHIVE (checked on a test client:
  `fps_max` default 150, `fps_max_nofocus` default 20, `snd_mute_losefocus` default 1; `engine_no_focus_sleep` is not
  a registered convar in this build), so a clean quit wrote them into `cfg/config.cfg`, which the user's own game shares:
  a user's `fps_max "240"` became `"20"` after one `game_quit`. The clients now carry `-mcp_test_client` (read by the
  module as `native.test_client`), and before one quits cleanly the module sets each of those convars back to the value
  `cfg/config.cfg` has at that moment (read from disk, the user's game may have written it since the launch), or to the
  convar's default when the file has no line. Covered: `game_quit`, `quit`/`exit` sent with `console_exec` or
  `engine_cmd`, the main menu's Quit button (`RunGameUICommand("quit")` is wrapped), and the window's close button
  (X, Alt+F4): the module subclasses the test client's window, queues the restore, holds the close request back 0.4 s
  and posts it again. Why not a hook at shutdown: the engine writes `config.cfg` before it closes the menu Lua state
  (`gmod13_close` runs after the write) and the `ShutDown` hook never runs in the menu state (both checked live).
  Not covered: a `quit` typed by hand in a test client's console, and a game started by hand. `instances_close` kills
  the process and writes nothing. A client that an older version started and quit may have left `fps_max` 20 in the
  user's config: set it again.
- **Test clients no longer overwrite the user's video settings.** `instances_launch` started its clients with
  `-w 640 -h 360`, and the engine wrote that mode (ScreenWidth, ScreenHeight, ScreenMSAA, ...) into
  `HKCU\Software\Valve\Source\garrysmod\Settings` at startup and at exit: the user's own game then
  started at 640x360 without anti-aliasing. Both writes are skipped when `-dxlevel` is on the command line (read in
  engine.dll, read-only), and the stored settings are then not read either, so the clients now carry `-dxlevel 95`, the
  level the engine keeps on current hardware. Settings damaged by an earlier version are not repaired: set the
  resolution again in the video options.
- **Starting a game could crash another game that was finishing a map load** (Garry's Mod issue 7137, issue #2).
  Starting any `gmod_win64.exe` makes every other running game release and restore its graphics resources for 2-10 s;
  a map load ending in that window crashes the game in `shaderapidx9`. The bridges of all your Claude sessions now
  share a lock: `instances_launch` waits while any game loads a map, and `map_load` waits while a game is being
  started and for 15 s after the last start. The wait is bounded by the call's `timeout_s` (error `map_load_in_progress`
  / `launch_in_progress`), reported as progress, and in the result (`waited_s`, `waited_reason`). A game started by
  hand is not seen. `instances_launch {map}` loads its maps after that delay and at most 2 at a time instead of all at
  once. Set `GMOD_MCP_FP7137_WORKAROUND=0` to turn it off. Everything is in `src/bridge/fp7137.*` and
  tagged `FP7137`, to be removed when the engine is fixed: see `docs/FP7137.md`.
  The `instances_launch` and `map_load` descriptions, the server instructions, the guide, the skill and the README now say it
  (what waits, `waited_s` / `waited_reason`, the two errors, what the bridge does not see, games unreachable for 15-18 s
  after a start, launch everything in one call first).

### Docs

- README, from a literal walk through its install steps: updating by hand needs `claude plugin marketplace update
  gmod-mcp` before `claude plugin update gmod-mcp@gmod-mcp` (alone it answers "already at the latest version"); `claude
  mcp add` takes `--scope user` (also in the text `mcpagent_setup` prints), otherwise the entry only exists in the
  folder where it was run; the plugin does not replace a manual `gmod` entry (both stay, tools listed twice: remove it
  with `claude mcp remove gmod`); the Linux zip is named (`gmod-mcp-linux64.zip`) and the top folder of both zips
  explained; `mcpagent_setup` and the plugin work with a dedicated server; uninstall keeps `-mcp` until
  `mcpagent_uninstall` has run, then removes it from the launch options, deletes `menu.lua.mcpagent.bak` and removes the
  marketplace; auto-update: no `claude plugin` subcommand turns it on (the `/plugin` menu does); the `feedback_offers`
  opt-out and the "1 userConfig option not yet set" notice; configuration (`GMOD_MCP_PORT` is what works with the
  plugin, `-mcpport` range, a base port held by another program); manual-path updates; the version warning shows a
  moment after the game first answers; `lua_function_source` in the tools table; the comparison has its own section; links
  that broke in the release zip and in the marketplace clone are absolute; the Linux token path honours
  `XDG_CONFIG_HOME`. The skill carries the update commands and the manual-entry facts.

## 0.3.5

A crash fix, then the findings of an independent review of everything since 0.3.1, each with a test seen failing
first, and checked in a real game.

### Fixes

- **The game could crash on `console_exec` after a map change** (since 0.3.2). `console_exec` reads back every convar it
  sets, through `convar_get` in the menu realm, and GMod's `GetConVar` caches the ConVar object per name. For a convar
  an addon re-creates at every map load, the cached object pointed to freed memory after the change: access violation.
  gmod-mcp now always asks the engine afresh (`GetConVar_Internal`) and reads a convar back in the realm that owns it.
- **`console_exec "echo x; connect ..."` sent `connect` while a map was loaded**, which crashes the engine: a line with
  `connect` is refused unless `connect` is alone on it.
- **Printing could throw into the engine**: the console capture code called by the engine's print functions now never
  lets an exception out, skips error parsing of lines over 4 KB, and parses frame numbers without exceptions.
- **A slow regex in `console_read` / `console_wait` froze the game**: it ran while holding the lock every engine print
  waits for. Lines are now copied, then matched outside the lock; a regex too costly to run is an error instead of a
  crashed thread.
- **Async job ids were reused after a map change**: `lua_job_status {id}` could return another map's job. Ids carry the
  realm's generation (`server-g3-1`); an old id answers `realm_reset`.
- **Memory could grow without limit**: finished async jobs nobody collected are now pruned even when no job runs, and
  each job report is bounded (depth, entries, string size, 32 MiB per job), cuts being counted.
- **`instances_launch {lua_mount}` reported a mount it never checked**: it now asks each client what its engine really
  has, and a mount that is not active is an error naming the clients. A folder with non-ASCII characters is refused:
  the game's file system lists it but loads none of its files (use an ASCII junction).
- **`instance_gone` could call a dead game "frozen"** when its pid had been reused: the process start time is compared.
- **`errors_since {since_map_load: true}` kept the wrong errors.** A map load now starts when the new server Lua state
  appears (the module is required in it by the autorun loader), in client and srcds modes, instead of when the pump later
  noticed the realm: errors raised while the map loaded were stamped with the previous map, and on a dedicated server the
  number never grew after the first map. Errors raised between the old server state closing and the new one opening
  (shared autorun files) are re-stamped to the new load. The `realm` and `since_map_load` filters now apply before
  `limit`: with more than `limit` older errors kept, the current map's errors were returned as an empty list.

### Tests

- Live regression cases (`test_bridge.py convar_crash`, `fixes_live`): the crash reproduced before the fix, 3/3 after;
  the review fixes 11/11 in a real game. The bridge suite runs its cases in parallel (29 s instead of 2 min 30).

### Docs

- Comparison with the other Garry's Mod MCP servers: a heatmap in the README and the full table in
  [docs/COMPARISON.md](https://github.com/ExtReMLapin/gmod-mcp/blob/main/docs/COMPARISON.md) (22 servers, every cell linked to its evidence at a pinned commit). It is
  generated from `docs/comparison/data.json` by `docs/comparison/render.py`.

### Upgrading

- Update the plugin (auto-update, or `claude plugin update gmod-mcp@gmod-mcp`), restart Claude Code, then let Claude
  update the module in the game (quit it first) when the version warning asks for it. Update soon if agents use
  `console_exec` on addon convars across map loads (the crash).

## 0.3.4

### New

- **Agents report their frictions with gmod-mcp, with the user's agreement.** The server instructions, the guide
  ("Reporting frictions") and the skill ask agents to note what got in their way with gmod-mcp (a wrong or unclear
  description, an error without a reason, a missing tool, a workaround they had to write) and, at the end of a task,
  to offer once to report what cost real time or a feature. The issue is public and in English: no secrets, paths,
  project names or code; it names the coding agent, the model and the versions. Agents look for duplicates, file it
  with `gh` when it is signed in, otherwise give a prefilled link. New issue template and `agent-feedback` label.
- **Plugin option `feedback_offers`** (on by default): turns these offers off. Set it with
  `/plugin configure gmod-mcp@gmod-mcp`, or ask the agent to stop offering: it sets the option for you. Restart
  Claude Code after a change.

### Upgrading

- Update the plugin (auto-update, or `claude plugin update gmod-mcp@gmod-mcp`), restart Claude Code, then let Claude
  update the module in the game (quit it first) when the version warning asks for it.

## 0.3.3

### Fixes

- **The version warning was missing from the bridge's own tools.** Seen in a real update from 0.3.1: "launch an
  instance" went through `instances_launch`, a tool of the bridge itself, whose answers did not carry the warning, so
  the agent only saw it later in a `status`. `instances_launch`, `instances_close` and `instance_select` now carry it,
  `instances_launch` checks the version of the clients it just started before answering, and the warning explains why
  tools listed while the game was closed can be missing (that list is the plugin's, the live one is the module's).
- CI: the Linux build ran every compile at once and the runner killed it; it now runs 4 at a time.

### Upgrading

- Update the plugin (auto-update, or `claude plugin update gmod-mcp@gmod-mcp`), restart Claude Code, then let Claude
  update the module in the game (quit it first) when the warning asks for it.

## 0.3.2

From the field feedback of agents that ran up to 17 test clients in parallel, then checked in game on a Windows
client, a Windows dedicated server and a Linux dedicated server (also all three behind one bridge).

### Fixes

- **A plugin newer than the module in the game went unnoticed.** Claude Code updates the plugin (bridge, skill, a copy
  of the module), not the module the game loads. The mismatch was only in the connection instructions, and only when
  the game was already running then. The bridge now asks the game its version whenever it becomes reachable, and while
  they differ every tool result starts with a warning that names both versions and says what to do.
- **Console capture was empty on the x86-64 Linux dedicated server** (its libtier0 has no `SpewOutputFunc`): no
  `console_exec` output, no `console_wait`, no Lua errors. gmod-mcp now listens to tier0's logging system there.
- **On a dedicated server, Lua errors in code without a .lua file** (`lua_exec`, `CompileString`) were not caught from
  the console.
- **A game whose gmod-mcp config folder differs was refused by the coordinator in silence** (another token: HTTP 401)
  and stayed invisible. It now logs why and how to fix it, and `status` gives `coordinator_link`.
- **Some tool descriptions were wrong or left agents guessing**: `screenshot` claimed to capture the main menu,
  `lua_reload` claimed a client `include()` ignores edits (it reads the disk on the hosting machine). Every tool now
  says what it does and does not do, what it needs, each parameter's unit and default, the result's shape, its side
  effects, with an example; facts the code could not settle were checked in game.
- **A console line could still connect to a remote server.** The network rule split the line its own way and kept a
  quote open across a line break, so `echo "a` + line break + `connect 1.2.3.4` passed as one `echo` but ran as an
  `echo` and a `connect`. It now checks exactly the statements that run.
- **Some text broke a whole answer.** The runtime let UTF-16 surrogates, overlong 3-byte sequences and code points above
  U+10FFFF through, which the module's JSON parser refuses: the call failed with `bad_runtime_output`. They are escaped
  like any invalid byte. `mcpagent://config` no longer fails on a game path that is not valid UTF-8.
- **The bridge accepted bad arguments for its own tools.** `instances_launch {count: 0}` answered with no client, and an
  `lua_mount` that is not a string or an unknown `preset` were ignored and real clients started. Arguments are now checked
  against each tool's schema first, and the error names the argument.
- lua_mount refuses a folder inside the game folder (garrysmod itself or an installed addon): the engine already
  searches it.
- **Test clients started together could lose gmod-mcp.** The crash guard was one marker file shared by every game: a
  client that started while another was loading took that for a crash and skipped gmod-mcp, and a real crash blocked
  every later launch until someone deleted the file. Each process now has its own marker, checked by the module: a
  load in progress elsewhere is ignored, and a real crash skips gmod-mcp for exactly one launch.
- **Sub-agents closed each other's clients.** Sub-agents of one Claude Code session share one bridge, so
  `instances_close {all: true}` closed all their clients and `instance_select` redirected all their calls.
  `instances_launch` takes an `owner` label, `instances_close {owner}` closes only those, and the descriptions and the
  skill say not to use `all` or `instance_select` with sub-agents.
- **Clients in which gmod-mcp never loaded could only be killed by hand.** `instances_launch` names them
  (`silent_pids`) when it times out, and `instances_close {pids}` closes them.
- `convar_set` retries through the engine console when Lua is silently ignored (`volume_sfx`), and says which way
  worked.
- `lua_check`, `lua_exec_file` and `lua_job` find relative paths in the game folder too (`addons/x/lua`), and a miss
  lists every place tried.
- The text of `mcpagent_setup` gives the right plugin install commands.
- **Results were cut without warning.** `lua_exec` and `lua_job` replaced any string over 64 KiB, and tables deeper
  than 4 levels or longer than 200 entries, by a stub; a script that read the stub as data saw an empty list. Values
  now come back whole. `depth`, `limit` and `max_string` cut only when asked, and the result then says
  `truncated: <count>`. Only a value too big to send (over a million values or 64 MiB of strings) fails with `result_too_large`; the client
  realm's whole `_G` (4.5 MB of JSON) comes back whole.
- **Code loaded through `lua_exec` ran several times slower than real.** The LuaJIT compiler was turned off for the
  chunk and for every function it defined (hooks, timers), for their whole life, so the time budget could interrupt
  loops; performance measured through such functions was wrong. gmod-mcp no longer turns the compiler off, and there
  is no time limit by default: an infinite loop freezes the game, the call fails with `main_thread_timeout` after
  `timeout_s` (default 60 s), `status` shows `main_thread_stalled` and `game_terminate` ends it. `budget` (and
  `lua_job`'s `resume_limit_s`) are now optional and best effort. `jit` is obsolete.
- **`lua_job` scripts that read `args` got nil** (the arguments are in `job.args`): `args` is now a global of the
  script too.
- **`console_exec "a 1;b 1"` set `a` to `"1;b 1"`.** The whole line went to the first command. It is now split into
  statements on `;` and line breaks outside quotes, like the engine console does, and each one runs in turn
  (`statements` lists them).
- **A mistyped command answered "ok".** The engine only prints `Unknown command: x`; `console_exec` now reads that
  line and fails with `unknown_command`.
- **`console_exec` output mixed in gmod-mcp's own log lines** (`[mcpagent] initialize ...`): they are left out, and
  `regex` filters what remains (the console is shared, so other prints of the same frames can still appear).
- **Nothing said whether a convar set through `console_exec` took.** Every convar it sets or names is read back
  (`convars`: value, number), with a warning for archived ones (FCVAR_ARCHIVE): they land in the user's
  `cfg/config.cfg` and are shared by every instance.
- **The same convar value read `"0.0"` or `"0.000000"`**, so snapshot comparisons flagged false changes:
  `convar_get` also returns `number`.
- **Errors raised before a map reload were easy to miss.** Each Lua error now carries `map_load` (a number that grows
  with every map load) and `map`; `errors_since {since_map_load: true}` keeps only the current map's, including those
  raised while it loaded. The buffer itself survives map loads.
- **Read-only tools failed with `ambiguous_instance` as soon as two games ran**, even `map_list`, which is the same for
  all: `map_list` (like `status` and the wiki tools) now works without `instance`.
- **A call to an instance that had died gave a bare network error**, then `unknown_instance`. It now says
  `instance_gone` with the pid and whether the process exited (relaunch) or is still running (frozen:
  `game_terminate`).
- **`instances_list` said "not reachable" when no game ran, and did not show clients that never answered.** Through
  the bridge it now answers with an empty list and a note, lists the clients it launched that never answered
  (`launched_not_answering`, close them with `instances_close {pids}`), gives each client's `owner` label and filters
  on `owner`.
- **`instances_launch` timed out without saying which clients answered**: the error lists them (`answered`) next to
  `silent_pids`.

### New

- `lua_function_source`: tells whether a function running in the game is the code of its file on disk (bytecode
  compared with the file compiled again), and where it now starts in the file. For test results after an edit.
- `fields` on `lua_exec`, `lua_exec_file` and `lua_job`: only these keys of returned tables come back.
- `chunkname` on `lua_exec`, `lua_exec_file` and `lua_job`: the source name that `debug.getinfo` and tracebacks show,
  e.g. an addon-relative name for code that identifies its caller by file.
- `lua_job {async: true}` returns a job id at once; `lua_job_status` polls it (or lists every job) and returns the
  result once finished; `lua_job_cancel` stops it. A job started while another runs in the same realm reports it
  (`concurrent_jobs`).
- `lua_job {fresh_env: true}`: the script's globals stay in its own table instead of leaking into the next job.
- `lua_exec` results give `chunk_bytes`, `chunkname` and the realm's `generation` (it changes on every map load).
- **Per-instance Lua mount** (`lua_mount`, `instances_launch {lua_mount}`): one game process loads Lua from a folder
  of your choice (e.g. an addon's git worktree) before every addon, at every map load, so a branch can be tested in
  one test client without touching the shared `addons/` folder, the other clients or the user's game. Agents used to
  re-inject their files by hand after every map load, and tests silently ran on the old code when they forgot.
  Windows only for now.
  gmod-mcp's own Lua tools follow it: `lua_exec_file`, `lua_job {path}`, `lua_check`, `lua_reload` and
  `lua_function_source` find the folder's files first, in the engine's order, and name them like the engine
  (`@lua/...`). The guide resource and the server instructions explain it to agents ("Mounted Lua folder").
- `instances_list` gives each instance's version, uptime, fps, a stalled flag and its Lua error count.
- `console_wait {regex, count, timeout_s}`: blocks until that many console lines match (the end line of a test batch,
  for example), with progress, and returns them with a cursor for the next wait. Replaces hand-written polling loops.
- `status` gives `fps_max` (20 on a light test client: per-frame behaviour differs from a normal game) and
  `host_timescale`, and takes `fields` to return only some keys. `instances_launch` returns the preset's `fps_max`
  with a note.
- `instances_launch` documents what its presets change (the 20 fps cap hides per-frame cost; `-nosound` changes
  `SoundDuration` of compressed WAVs) and that clients share `data/` and cookies with the user's game.

### Upgrading

- Update the plugin: with auto-update on (`/plugin` → Marketplaces → gmod-mcp → Enable auto-update) it comes by itself;
  otherwise `claude plugin update gmod-mcp@gmod-mcp`. Then restart Claude Code.
- Then update the module in the game (quit it first): ask Claude to update gmod-mcp. Until then every tool result
  starts with the version warning (module 0.3.1, plugin 0.3.2).

### Tests

- Native unit tests without a game (`gmod-mcp-unittests`, `gmod-mcp-coretests`), and a stand-in game for the bridge
  tests, all in CI on Windows and Linux.
- Lua runtime tests without a game (`tests/lua/runtime_test.lua`, plain LuaJIT 2.1.0-beta3 like the game's, in CI on
  Linux): whole values, explicit cuts flagged, size guard, chunk names, the compiler left on for defined functions,
  explicit budgets, job arguments, fresh environments, concurrent and async jobs.
- No-game checks of the console helpers (statement splitting, unknown commands, own log lines), `console_wait`, error
  map-load tags, `status` fields, instance-independent tools and the dead-instance message; bridge checks of
  `instances_list` without a game, with silent clients and with an owner filter.

## 0.3.1

### Fixes

- **Setup skill, Windows**: the command that finds the module shipped with the plugin called `gmod-mcp-bridge`, which
  Git Bash resolves to the Linux build of the bridge (same folder, no extension), so it failed with "Exec format error".
  It now calls `gmod-mcp-bridge.exe` on Windows. Found by an end-to-end run of the published 0.3.0 plugin: install from
  the marketplace, module installed from the plugin, game started, tools through the bridge, `instances_launch`.

### Upgrading

- Update the plugin (`/plugin`, Marketplaces, update gmod-mcp, or wait for the auto-update) and restart Claude Code.
- Then update the module in the game (quit it first): ask Claude to update gmod-mcp. Until then the bridge reports a
  version mismatch (module 0.3.0, plugin 0.3.1); the module itself has no functional change.

## 0.3.0

### Highlights

- **Claude Code stays connected while the game is closed.** The plugin now starts `gmod-mcp-bridge`, a small stdio
  server, as the `gmod` MCP server. With the game closed, every tool is listed and answers that the game is not
  running; as soon as the game starts, the tools work, with no `/mcp` reconnect. The token never lands in Claude Code's
  settings. Several Claude Code sessions each get their own bridge.
- **Test clients on demand** (Windows): `instances_launch` starts 1 to 8 extra clients (small window, frame cap, map
  loaded through `map_load`), `instances_close` closes the ones this session started, `instance_select` sets the
  session's default instance. They close when the session ends.
- **One plugin, one zip per release**: skill, bridge and module for both platforms, pinned by SHA-256 in the
  marketplace. The skill installs the module shipped with the plugin.

### New tools and options

- `lua_reload`: runs an addon or gamemode file again from the disk in its realms. On the client, `include()` runs the
  copy the server sent at map load, so edits were silently ignored.
- `lua_check`: compiles a snippet, a file or a whole addon folder without running it; syntax errors with their line,
  unknown globals (typos, missing includes, wrong realm) and global assignments (a forgotten `local`).
- `jit: true` on `lua_exec` and `lua_job`: keeps the LuaJIT compiler on for performance measurements. By default it is
  off for the code and every function it defines, which made timings taken through hooks and timers several times
  slower than real.
- Invalid arguments now name the unknown and missing ones, suggest the closest accepted name (`command` → `cmd`) and
  list what the tool accepts. Syntax errors of `lua_exec` and `lua_job` carry their line.

### Fixes

- **Mouse grabbed by a background client**: a client that loaded a map while another application had the focus
  believed it had the focus and kept pulling the cursor to its window. The module now tells it the focus is elsewhere.
- **Linux: the module crashed the dedicated server on load** (its C++ exception handling called the game's own
  runtime). It now keeps its runtime private, and no start-up error can reach the game. Tested in a real Linux server.
- Paths returned to the agent use one separator style.

### MCP specification (2026-07-28)

Checked with the official conformance suite (`@modelcontextprotocol/conformance`); every applicable scenario passes, on
Windows and Linux. Fixed along the way: requests missing required `_meta` fields (-32602), header/body version mismatch
(-32020), `initialize` and `ping` removed in 2026-07-28 (404, -32601), local `Origin` headers accepted (others still
refused), strict `Mcp-Param-*` validation, Tasks methods without the Tasks capability (-32021). Protocol constants live in
one header, checked in CI against the official schema.

### Upgrading

- Install or update the plugin: `/plugin marketplace add ExtReMLapin/gmod-mcp` then `/plugin install gmod-mcp@gmod-mcp`
  (third-party marketplaces do not auto-update by default: enable it in `/plugin`, Marketplaces). Restart Claude Code.
  If you added the server by hand earlier (`claude mcp add ... gmod`), remove that entry.
- Update the module in the game (quit the game first): ask Claude to update gmod-mcp, or copy the two files from the
  release zip as before. The bridge warns when the module and the plugin versions differ.
- **Test clients and your resolution**: Garry's Mod saves the window size it starts with as its video setting. After an
  agent ran test clients, set your resolution back in the video options.
- Dedicated servers on Linux need the `x86-64` beta branch (the default branch has no 64-bit server).

### Release files

- `gmod-mcp-plugin.zip`: the Claude Code plugin (installed by `/plugin install`, not by hand).
- `gmod-mcp-win64.zip`, `gmod-mcp-linux64.zip`: the module to drop into the game or server folder, plus
  `gmod-mcp-bridge` for MCP clients other than Claude Code.

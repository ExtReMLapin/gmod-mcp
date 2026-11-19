# Changelog

## Unreleased

### Fixes

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

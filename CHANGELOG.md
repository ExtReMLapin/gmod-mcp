# Changelog

## Unreleased

From the field feedback of agents that ran up to 17 test clients in parallel.

### Fixes

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

### New

- `lua_function_source`: tells whether a function running in the game is the code of its file on disk (bytecode
  compared with the file compiled again), and where it now starts in the file. For test results after an edit.
- `fields` on `lua_exec`, `lua_exec_file` and `lua_job`: only these keys of returned tables come back.
- `instances_list` gives each instance's version, uptime, fps, a stalled flag and its Lua error count.
- `instances_launch` documents what its presets change (the 20 fps cap hides per-frame cost; `-nosound` changes
  `SoundDuration` of compressed WAVs) and that clients share `data/` and cookies with the user's game.

### Tests

- Native unit tests without a game (`gmod-mcp-unittests`, `gmod-mcp-coretests`), and a stand-in game for the bridge
  tests, all in CI on Windows and Linux.

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

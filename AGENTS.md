# Notes for coding agents working on this repository

gmod-mcp is a native Garry's Mod module (C++17) that embeds an MCP server and drives the game's Lua states. Read
`README.md` first for what it does; this file lists the rules that are easy to break.

## Layout

- `src/*.cpp|h` — the module. Entry point `module.cpp` (`gmod13_open/close`), MCP layer `mcp.cpp`, tools `tools_*.cpp`,
  engine access `realms.cpp` / `netaddr.cpp` / `luaapi.cpp`, OS-specific code only in `src/platform/`.
- `src/lua/*.lua` — Lua runtime injected into each realm, embedded into the DLL at build time
  (`cmake/embed_lua.cmake`, alphabetical order).
- `src/installer.cpp` — the main-menu loader block the module writes into `garrysmod/lua/menu/menu.lua`.
- `loader/mcpagent_boot.lua` — server autorun shipped in the release zip.
- `tests/conformance/` — the MCP conformance suite harness: `gmod-mcp-conformance-host` (the module's MCP server code plus
  the suite's fixtures, no game), `run.py`, `check_fixtures_absent.py`. Test-only: nothing in it ships.
- `docs/TESTS.md` — test matrix; `docs/REVERSE.md` — engine interfaces, vtable slots and offsets in use.

## Rules

- **English only**: code, comments, log messages, tool descriptions, docs.
- **No AI attribution** in commits (no `Co-Authored-By`, no session links).
- **Conformance fixtures live only in `tests/conformance/`**, and only the `gmod-mcp-conformance-host` executable links
  them. `src/` never calls `mcp::set_request_hooks` (a test-only seam for the conformance host; only its declaration and
  definition in `src/mcp.h` / `src/mcp.cpp` may name it). `tests/conformance/check_fixtures_absent.py` enforces both in CI,
  strongest guarantee first: the CMake build graph (File API: no `tests/conformance/` source in a target other than the
  host), then the shipped sources (no fixture name literal, no `set_request_hooks` call), then a binary scan of the module
  and bridge (secondary: a compiler can hide short strings) controlled by a long canary string that only the host defines.
- **Release checksum**: `loader/mcpagent_boot.lua` contains the placeholder `@MCPAGENT_DLL_CRC@`. Never replace it by
  hand: the CI stamps it with the CRC32 of the DLL it just built. Local builds keep the placeholder (check skipped).
- **Menu loader block**: when you change `kBlockSource` in `src/installer.cpp`, bump its `vN` so installed copies are
  rewritten. It must stay harmless: everything inside `pcall`, never break the main menu.
- **Never cache** a client/server `ILuaInterface*` or `lua_State*` (they are recreated on every map); always go through
  `realms::state()`.
- **All Lua runs on the main thread** (job queue + pump). The network thread must never touch Lua.
- **Security invariants**, not negotiable: bind 127.0.0.1 only; bearer token; reject `Origin`; client realm lock (no Lua
  in the client state unless the server runs on this machine); no connection to remote servers; the only outbound
  traffic is GET to `https://wiki.facepunch.com/gmod/`.
- **Long-running tools** (anything that waits or loops) must poll `reqctx::cancelled()` and report
  `reqctx::progress(done, total, message)`: this feeds MCP progress notifications (SSE), task status messages and
  cancellation (closed stream, `tasks/cancel`, legacy `notifications/cancelled`).
- **Error codes** returned by tools (`realm_unavailable`, `client_locked`, `ambiguous_instance`…) are part of the API:
  do not rename them.
- **Lua sources** contain escape sequences (`"\n"`): edit them with a real editor, not shell heredocs or `sed`, which
  turn them into raw newlines.
- **Comparison table** (`docs/comparison/data.json`): when a change adds, removes or limits something that matches a
  column (definitions in `docs/COMPARISON.md`; e.g. Linux support for `instances_launch`, a new realm, a debugger),
  update the `ExtReMLapin/gmod-mcp` row in a follow-up commit: re-pin the row's `sha` to the feature commit, re-check
  every evidence link of the row at that SHA, set the verdict by the definition with a line that proves it (no green
  without a line; partial if the README states a limitation), run `python docs/comparison/render.py`, and commit
  `data.json` with the regenerated SVGs and `docs/COMPARISON.md`. Do not change other servers' rows without
  re-checking them at a new pinned SHA.

## Build and test loop

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # dependencies are fetched and SHA-256 checked
cmake --build build --config Release
```

1. Quit the game (the DLL is locked while it runs).
2. Copy `build/gmsv_mcpagent_win64.dll` to `garrysmod/lua/bin/` and `loader/mcpagent_boot.lua` to
   `garrysmod/lua/autorun/server/`.
3. Start the game with `-mcp -windowed -condebug` (launching `gmod_win64.exe` directly needs the environment variables
   `SteamAppId=4000` and `SteamGameId=4000`).
4. Drive it through the MCP endpoint (`http://127.0.0.1:27080/mcp`, token in `%APPDATA%\gmod-mcp\token`), e.g. with curl.
   `events_wait {types:["menu_ready"], cursor:0}` returns as soon as the module is up.
5. Update `docs/TESTS.md` (scenario, expected result, last result and date) for any behavior you change.

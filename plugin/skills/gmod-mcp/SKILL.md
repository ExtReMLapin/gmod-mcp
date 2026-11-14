---
name: gmod-mcp
description: Use when a task involves Garry's Mod (GMod, gmod, GLua, a GMod dedicated server / srcds) and the `gmod` tools answer `game_not_running`, are missing, or the game is not running; when the user wants to install, update, start or connect gmod-mcp; or before telling the user that the game or its MCP server is unavailable. The MCP server lives inside the game, so it only answers while Garry's Mod runs with -mcp: starting the game is your job, not a dead end.
---

# gmod-mcp: install, start and connect

gmod-mcp is an MCP server **inside** Garry's Mod (a native module, https://github.com/ExtReMLapin/gmod-mcp). It only
answers while the game runs with the `-mcp` launch option. This plugin starts a small bridge as the `gmod` MCP server:
it stays connected while the game is closed (its tools then answer `game_not_running`) and relays to the game as soon as
it answers, so the tools come back by themselves, with no reconnection. When the tools say `game_not_running`, start
the game yourself (section 2) instead of reporting a dead end.

Once the server is connected, its own instructions (sent by the server) take over: follow them for everything else.

## Rules

- **Local only.** The server listens on `127.0.0.1` (base port 27080, then 27081... for more games). Never connect the
  game to a remote server, never expose the port.
- **Ask before installing or updating**: it adds two files to the user's game folder.
- **Do not restart a game the user closed** in this conversation, or one they said to leave closed: ask first.
- Never kill Steam, never change the game's beta branch, never edit the user's addons, `cfg` or saves.
- Never print the token in the conversation. Read it straight from the file into the command that needs it.

## 1. Find out where things stand

1. **One `gmod` server only.** The plugin provides it. An older install may also have a manual entry: if
   `claude mcp get gmod` shows an `http://127.0.0.1:27080/mcp` server, tell the user it duplicates the plugin's and
   remove it with their agreement (`claude mcp remove gmod`, adding `--scope` if it lives in another scope).
2. **Is a game listening?** Any HTTP answer means yes (401 without the token is the expected one):

   ```bash
   curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:27080/mcp     # 401 = up, 000 = nothing listening
   ```

   On Windows PowerShell, call `curl.exe`, not `curl`.
3. **Is the module installed?** In the game folder (see below):
   `garrysmod/lua/bin/gmsv_mcpagent_win64.dll` (Linux: `gmsv_mcpagent_linux64.dll`) and
   `garrysmod/lua/autorun/server/mcpagent_boot.lua`.

**Finding the game folder.** Garry's Mod is Steam app 4000, in one of Steam's libraries:

- Windows: Steam's folder is `SteamPath` in `HKCU\Software\Valve\Steam`
  (`reg query "HKCU\Software\Valve\Steam" /v SteamPath`), usually `C:\Program Files (x86)\Steam`.
- Linux: `~/.local/share/Steam` or `~/.steam/steam`.
- Every library is listed in `<Steam>/steamapps/libraryfolders.vdf` (`"path"` entries). The game is the library whose
  `steamapps/appmanifest_4000.acf` exists, in `<library>/steamapps/common/GarrysMod`.

## 2. Start the game (server down, module installed)

Steam must be running and logged in. If it is not, start it (`steam.exe` / `steam`) and wait until it is ready; never
kill it.

**Windows** (64-bit game, `gmod_win64.exe` at the root of the game folder). Start it directly; the two variables are
required, otherwise the game stops with "No appID found":

```powershell
$env:SteamAppId = "4000"; $env:SteamGameId = "4000"
Start-Process -FilePath "<game folder>\gmod_win64.exe" -WorkingDirectory "<game folder>" `
  -ArgumentList "-mcp -windowed -w 1280 -h 720 -novid -condebug +engine_no_focus_sleep 0 +snd_mute_losefocus 0"
```

If `gmod_win64.exe` is missing, the game is on 32-bit binaries: tell the user (the module is 64-bit only) and stop.

**Linux**, or as a fallback on Windows: `steam -applaunch 4000 -mcp -windowed -novid +engine_no_focus_sleep 0
+snd_mute_losefocus 0` (on Windows, `"<Steam>\steam.exe" -applaunch 4000 ...`). Steam adds the user's own launch options.

- `-mcp` is the one that matters: without it the module does nothing.
- `+engine_no_focus_sleep 0 +snd_mute_losefocus 0` keep the game at full speed while it is not in focus.
- **Very first start after an install**: add `+map gm_construct`. The module installs its main-menu loader during that
  first map load; later starts need no map (the server is up at the main menu).

Then wait for the server: check the URL from section 1 every 2 seconds, for up to 2 minutes (the first start and a map
load take longer). Run the wait in the background or in one bounded loop, not as many separate calls.

**When it answers**: call the `gmod` tools again. The bridge notices the game within 2 seconds and Claude Code
fetches the live tool list by itself; the guide is the resource `mcpagent://guide`. If the server instructions say the
module and the plugin versions differ, tell the user and offer section 3 (update).

**Several clients side by side** (for example to split a long test suite): use the `instances_launch` tool (Windows),
which works even while no game runs. It starts 1 to 8 clients with the right options (`-multirun`, a small window, a
frame cap, no `+map`), waits until each answers, loads the map with `map_load`, and returns their ids and pids. Then
pass `instance` to every tool, or `instance_select` to make one the session's default (with several games running,
calls without `instance` are refused with `ambiguous_instance`). Close them with `instances_close`; they also close
when the session ends. Its description lists the measured cost per client and the limits (memory is what runs out
first).

- **With sub-agents**: they all share one bridge, so one session. Never use `instance_select` (its default would apply
  to every sub-agent's calls) and never `instances_close {all: true}` (it would close the others' clients). Tell each
  sub-agent in its brief to launch with an `owner` label of its own, pass `instance` on every call, and close with
  `instances_close {owner}`.
- **Testing a branch of an addon** (a git worktree): launch the client with `instances_launch {overlay: "<worktree>"}`
  (or call `lua_overlay {path}` then `map_load` on a running one). That game then loads the worktree's Lua before the
  installed addon, at every map load; other games are not affected. Do not re-inject files with `lua_exec` after each
  map load, and check with `lua_function_source` that the function under test comes from the worktree. Windows only.
- A client that never answers (gmod-mcp did not load in it): `instances_launch` returns its pid in `silent_pids`;
  close it with `instances_close {pids}`.
- The clients share the game folder with the user's own game: addons, cfg, `data/` and cookies (`sv.db`/`cl.db`). A
  test that writes cookies, data files or archived convars changes the user's game too: clean up after it. Archived
  convars (FCVAR_ARCHIVE) are saved to `cfg/config.cfg` and seen by every instance: one agent's `pba_x 0` left behind
  pollutes another's runs (`console_exec` warns when it sets one; `convar_set` refuses them unless `persist: true`).
- A background test client may believe it has the focus (`system.HasFocus()` true): `fps_max_nofocus` is not
  exercised there.
- Engine facts that cost agents runs: entity names (targetname) are server-side (`ents.FindByName` does nothing on the
  client; `Entity:GetName`/`SetName` are server-only); `EyePos()`/`EyeAngles()` are only reliable inside rendering
  hooks (use `LocalPlayer():EyePos()` in a `lua_job`).

- **Tell the user before launching**: Garry's Mod saves the window size of these clients as its own video setting, so
  their next normal start opens at that size until they set their resolution back in the video options. Closing the
  clients does not undo it.
- If the tool cannot find the game, pass `game_dir` (see "Finding the game folder" above).
- On Linux, start extra clients by hand with `-multirun` and load maps with `map_load` and the client's `instance`
  (not `+map`: a game that loads a map while another window has the focus can grab the mouse).
- An unfocused game is capped at 20 fps by default (`fps_max_nofocus`); the launched clients set their own cap.

## 3. Install (or update) the module

The plugin ships the module of its own version. Ask the user first; say that it adds two files to the game folder, and
that on the first start with `-mcp` the module also appends a small loader block to `garrysmod/lua/menu/menu.lua` (the
original is saved as `menu.lua.mcpagent.bak`; the game console command `mcpagent_uninstall` removes it).

1. Quit the game first if it is running (an update cannot replace a loaded DLL): ask the user to quit it, or use the
   `game_quit` tool.
2. Find the module shipped with the plugin. The plugin's `bin/` folder is on the PATH of the Bash tool:

   ```bash
   # Windows: name the .exe (the bin/ folder also holds the Linux binary, which Git Bash would pick first and fail on)
   src=$(gmod-mcp-bridge.exe --module-dir | tr -d '\r')   # Linux: src=$(gmod-mcp-bridge --module-dir)
   ls "$src/garrysmod/lua/bin" "$src/garrysmod/lua/autorun/server"
   ```

3. Copy these two files into the game folder at the same relative paths, creating `lua/bin` if needed, and nothing
   else:
   - `garrysmod/lua/bin/gmsv_mcpagent_win64.dll` (Linux: `gmsv_mcpagent_linux64.dll`)
   - `garrysmod/lua/autorun/server/mcpagent_boot.lua`
4. Start the game as in section 2, **with `+map gm_construct`** this first time. The game console says
   `main-menu loader installed in lua/menu/menu.lua`. The `gmod` tools come back by themselves.
5. Optionally, suggest Steam launch options (Garry's Mod → Properties → Launch options), so the server is also up, at
   full speed, when the user starts the game themselves:
   `-mcp -windowed +engine_no_focus_sleep 0 +snd_mute_losefocus 0`. `-mcp` starts the server; without
   `+engine_no_focus_sleep 0` the game throttles itself while another window has the focus (the usual case while
   the user is in the terminal), so every call that waits on frames gets slower; `+snd_mute_losefocus 0` keeps
   sounds playing for sound tests. `-windowed` is a preference, so the terminal stays reachable. Do not edit Steam's
   files to set them: tell the user.

If `gmod-mcp-bridge` is not found, the plugin is an old version without the bridge: ask the user to update it
(`/plugin`, Marketplaces, update gmod-mcp) and restart Claude Code.

**Other MCP clients** (not Claude Code): in the game console, `mcpagent_setup` prints an HTTP configuration with the
token. The bridge also works for them as a stdio server: the command `gmod-mcp-bridge` (from the plugin, or from the
release), with `--port <n>` or the `GMOD_MCP_PORT` variable for another base port.

## Dedicated server (srcds)

The same module works in a Garry's Mod dedicated server (Steam app **4020**, a separate install from the game). Use it
when the user works on a server-side addon or gamemode, or asks for a dedicated server. Only the server realm exists
there (no menu, no client, no screenshots).

**Install the server** (ask first: it is a download of several GB, and the user picks the folder). With SteamCMD
(https://developer.valvesoftware.com/wiki/SteamCMD), in a folder of its own:

```bash
steamcmd +force_install_dir "<server folder>" +login anonymous +app_update 4020 -beta x86-64 validate +quit
```

`-beta x86-64` matters: the module is 64-bit, and on Linux the default branch of the dedicated server only has the
32-bit `srcds_linux` (no `srcds_run_x64`). This is a separate install, not the user's game.

If the user already has a server, use theirs: it is the folder holding `srcds_win64.exe` (Linux: `srcds_run_x64`) and a
`garrysmod` folder. If that 64-bit binary is missing, the server is on the 32-bit branch: ask the user before switching
it to `-beta x86-64`.

**Install the module**: as in section 3, the same two files, into `<server folder>/garrysmod/...`.
A server has no main menu: it needs no `+map gm_construct` "first start", but it needs a map on every start.

**Start it** with the **64-bit** binary (`srcds.exe` and `srcds_run` are 32-bit: the module does not load there):

```powershell
Start-Process -FilePath "<server folder>\srcds_win64.exe" -WorkingDirectory "<server folder>" `
  -ArgumentList "-console -mcp -condebug -port 27015 +maxplayers 2 +sv_lan 1 +map gm_construct"
```

Linux: `./srcds_run_x64 -console -mcp -condebug -port 27015 +maxplayers 2 +sv_lan 1 +map gm_construct`. Keep its standard
input open when starting it from a script (for example `sleep infinity | ./srcds_run_x64 ...`): with `-console`, an
end-of-file on standard input can stop it.

- `+sv_lan 1` keeps the server off the public server list. Never open or forward its ports. If Windows asks about the
  firewall, the MCP port needs nothing (it is loopback); allowing private networks is enough to join from this machine.
- Wait for it as in section 2 (`curl` on 27080, or 27081 when a client game already holds 27080).
- The token is the same file as the game's (same user), and `mcpagent_setup` works in the server console.

**Reaching it from the agent.** A server alone takes port 27080, so the usual `gmod` server works. With the client
game also running, each has its own port and the one holding 27080 relays to the other: keep the single `gmod` entry,
call `instances_list`, then pass `instance` (`srcds-<pid>`) to any tool. Calls without `instance` are refused with
`ambiguous_instance` while several games run.

**Joining it from the local game**: `connect <this machine's LAN address>:27015` (in `console_exec`, or the game
console). A GMod dedicated server ignores connections from `127.0.0.1`, so use the LAN address (`ipconfig` /
`ip addr`). The client realm stays available because the server runs on this machine.

**Stopping it**: the `game_quit` tool with its `instance`, or `quit` in its console. Leave a server the user started
themselves running unless they ask.

## When it still does not answer

- `garrysmod/console.log` (written thanks to `-condebug`): the module's lines start with `[mcpagent]`.
  `active: ... http://127.0.0.1:<port>/mcp` means the server is up on that port; `inactive: the -mcp flag is not on the
  command line` and `refused: ...` say why it is not.
- No `[mcpagent]` line at all on the very first start: the module only loads with a map; start again with
  `+map gm_construct`.
- The process is running but the URL gives `000`: wait for the main menu or the map load to finish.
- Another program holds port 27080: the module takes the next free one (the `active:` line names it). The bridge
  only talks to the base port: ask the user to set the `GMOD_MCP_PORT` environment variable to that port and restart
  Claude Code, or better, to free port 27080.

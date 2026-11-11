---
name: gmod-mcp
description: Use when a task involves Garry's Mod (GMod, gmod, GLua, a GMod dedicated server / srcds) and the `gmod` MCP server is missing, failed to connect, or answers "connection refused"; when the user wants to install, update, start or connect gmod-mcp; or before telling the user that the game or its MCP server is unavailable. The MCP server lives inside the game, so it only exists while Garry's Mod runs with -mcp: starting the game is your job, not a dead end.
---

# gmod-mcp: install, start and connect

gmod-mcp is an MCP server **inside** Garry's Mod (a native module, https://github.com/ExtReMLapin/gmod-mcp). It only
exists while the game runs with the `-mcp` launch option. When the `gmod` MCP server is down, the usual reason is simply
that the game is not running: start it yourself (section 2) instead of reporting a dead end.

Once the server is connected, its own instructions (sent by the server) take over: follow them for everything else.

## Rules

- **Local only.** The server listens on `127.0.0.1` (base port 27080, then 27081... for more games). Never connect the
  game to a remote server, never expose the port.
- **Ask before installing or updating**: it adds two files to the user's game folder.
- **Do not restart a game the user closed** in this conversation, or one they said to leave closed: ask first.
- Never kill Steam, never change the game's beta branch, never edit the user's addons, `cfg` or saves.
- Never print the token in the conversation. Read it straight from the file into the command that needs it.

## 1. Find out where things stand

1. **Is the `gmod` MCP server configured?** `claude mcp get gmod`. If it is not, finish section 3 or 4 first.
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

**When it answers**: if the `gmod` tools were already in your tool list, call them again: the server keeps no session,
so calls work again as soon as the game is back. If they are not in your tool list (the server failed to connect when
this session started), ask the user to reconnect it with `/mcp` (select `gmod`, Reconnect), or to restart Claude Code.

**Several clients side by side** (for example to split a long test suite): start each extra game with `-multirun`, a
small window and a frame cap so they stay cheap:

```powershell
$env:SteamAppId = "4000"; $env:SteamGameId = "4000"
Start-Process -FilePath "<game folder>\gmod_win64.exe" -WorkingDirectory "<game folder>" `
  -ArgumentList "-multirun -mcp -windowed -w 960 -h 540 -novid +fps_max 45 +fps_max_nofocus 45"
```

- Load the map with `map_load` and its `instance`, not `+map`: a game that loads a map while another window has the
  focus can grab the mouse.
- Each game takes the next port (27081, 27082...) and registers with the one on 27080: keep the single `gmod` entry,
  call `instances_list`, and pass `instance` to every tool (calls without it are refused with `ambiguous_instance`).
- Close the extra games with `game_terminate`, not `game_quit`: a clean quit writes their window size and `fps_max`
  over the user's own config.
- An unfocused game is capped at 20 fps by default (`fps_max_nofocus`); pass `+fps_max_nofocus 0` when a test measures
  frame times.

## 3. Install (or update) the module

Ask the user first; say that it adds two files to the game folder, and that on the first start with `-mcp` the module
also appends a small loader block to `garrysmod/lua/menu/menu.lua` (the original is saved as
`menu.lua.mcpagent.bak`; the game console command `mcpagent_uninstall` removes it).

1. Quit the game first if it is running (an update cannot replace a loaded DLL): ask the user to quit it, or use the
   `game_quit` tool if it is connected.
2. Download the latest release, Windows `gmod-mcp-win64.zip`, Linux `gmod-mcp-linux64.zip`, and its `.sha256`, into a
   new empty temporary folder:

   ```bash
   base=https://github.com/ExtReMLapin/gmod-mcp/releases/latest/download
   curl -fsSLO "$base/gmod-mcp-win64.zip" && curl -fsSLO "$base/gmod-mcp-win64.zip.sha256"
   ```

3. Check the checksum (`sha256sum gmod-mcp-win64.zip` against the hash in the `.sha256` file; PowerShell:
   `Get-FileHash -Algorithm SHA256`). Stop if it differs.
4. Unzip it. It holds `gmod-mcp-win64/garrysmod/...`: copy these two files into the game folder at the same relative
   paths, creating `lua/bin` if needed, and nothing else:
   - `garrysmod/lua/bin/gmsv_mcpagent_win64.dll` (Linux: `gmsv_mcpagent_linux64.dll`)
   - `garrysmod/lua/autorun/server/mcpagent_boot.lua`
5. Start the game as in section 2, **with `+map gm_construct`** this first time. The game console says
   `main-menu loader installed in lua/menu/menu.lua`.
6. Optionally, suggest Steam launch options (Garry's Mod → Properties → Launch options), so the server is also up, at
   full speed, when the user starts the game themselves:
   `-mcp -windowed +engine_no_focus_sleep 0 +snd_mute_losefocus 0`. `-mcp` starts the server; without
   `+engine_no_focus_sleep 0` the game throttles itself while another window has the focus (the usual case while
   the user is in the terminal), so every call that waits on frames gets slower; `+snd_mute_losefocus 0` keeps
   sounds playing for sound tests. `-windowed` is a preference, so the terminal stays reachable. Do not edit Steam's
   files to set them: tell the user.

## 4. Connect Claude Code

The module creates its token on its first start with `-mcp`: `%APPDATA%\gmod-mcp\token` on Windows,
`~/.config/gmod-mcp/token` on Linux. With the game started once, register the server for every project of this user,
reading the token from the file without echoing it:

```bash
# Windows (Git Bash): token file "$APPDATA/gmod-mcp/token"; Linux: ~/.config/gmod-mcp/token
claude mcp add --scope user --transport http gmod http://127.0.0.1:27080/mcp \
  --header "Authorization: Bearer $(tr -d '\r\n' < "$APPDATA/gmod-mcp/token")"
```

If a `gmod` entry already exists (`claude mcp get gmod`) with a different URL or token, `claude mcp remove gmod` first
(add `--scope` if it lives in another scope). The new server shows up after `/mcp` → Reconnect, or a restart of Claude
Code: tell the user.

In the game console, `mcpagent_setup` prints the same command, plus a `.mcp.json` snippet for other MCP clients.

## Dedicated server (srcds)

The same module works in a Garry's Mod dedicated server (Steam app **4020**, a separate install from the game). Use it
when the user works on a server-side addon or gamemode, or asks for a dedicated server. Only the server realm exists
there (no menu, no client, no screenshots).

**Install the server** (ask first: it is a download of several GB, and the user picks the folder). With SteamCMD
(https://developer.valvesoftware.com/wiki/SteamCMD), in a folder of its own:

```bash
steamcmd +force_install_dir "<server folder>" +login anonymous +app_update 4020 validate +quit
```

If the user already has a server, use theirs: it is the folder holding `srcds_win64.exe` (Linux: `srcds_run_x64`) and a
`garrysmod` folder.

**Install the module**: as in section 3, the same two files from the same zip, into `<server folder>/garrysmod/...`.
A server has no main menu: it needs no `+map gm_construct` "first start", but it needs a map on every start.

**Start it** with the **64-bit** binary (`srcds.exe` and `srcds_run` are 32-bit: the module does not load there):

```powershell
Start-Process -FilePath "<server folder>\srcds_win64.exe" -WorkingDirectory "<server folder>" `
  -ArgumentList "-console -mcp -condebug -port 27015 +maxplayers 2 +sv_lan 1 +map gm_construct"
```

Linux: `./srcds_run_x64 -console -mcp -condebug -port 27015 +maxplayers 2 +sv_lan 1 +map gm_construct` (untested so far).

- `+sv_lan 1` keeps the server off the public server list. Never open or forward its ports. If Windows asks about the
  firewall, the MCP port needs nothing (it is loopback); allowing private networks is enough to join from this machine.
- Wait for it as in section 2 (`curl` on 27080, or 27081 when a client game already holds 27080).
- The token is the same file as the game's (same user), and `mcpagent_setup` works in the server console.

**Reaching it from the agent.** A server alone takes port 27080, so the usual `gmod` entry works. With the client
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
- Another program holds port 27080: the module takes the next free one (the `active:` line names it). Point the `gmod`
  entry at it only if no other game is running.

---
name: gmod-mcp
description: Use when a task involves Garry's Mod (GMod, gmod, GLua) and the `gmod` MCP server is missing, failed to connect, or answers "connection refused"; when the user wants to install, update, start or connect gmod-mcp; or before telling the user that the game or its MCP server is unavailable. The MCP server lives inside the game, so it only exists while Garry's Mod runs with -mcp: starting the game is your job, not a dead end.
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
6. Optionally, tell the user they can add `-mcp` to the game's Steam launch options, so the server is also up when
   they start the game themselves.

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

## Dedicated server (only if the user asks for one)

Same two files, in the dedicated server's own `garrysmod` folder. Start it with `-mcp` and a map, for example
`srcds_win64.exe -console -game garrysmod +maxplayers 2 +map gm_construct -mcp`. With a client game already running, the
server takes the next port and registers with the game holding 27080: the agent reaches it through the same `gmod` entry
(`instances_list`, then `instance` on any tool). Only the server realm exists there.

## When it still does not answer

- `garrysmod/console.log` (written thanks to `-condebug`): the module's lines start with `[mcpagent]`.
  `active: ... http://127.0.0.1:<port>/mcp` means the server is up on that port; `inactive: the -mcp flag is not on the
  command line` and `refused: ...` say why it is not.
- No `[mcpagent]` line at all on the very first start: the module only loads with a map; start again with
  `+map gm_construct`.
- The process is running but the URL gives `000`: wait for the main menu or the map load to finish.
- Another program holds port 27080: the module takes the next free one (the `active:` line names it). Point the `gmod`
  entry at it only if no other game is running.

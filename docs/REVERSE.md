# Reverse-engineering notes (interoperability)

Binaries analyzed: those installed on the development machine (GMod `buildid 25375506`, `PatchVersion=2026.04.29`).
No decompiled code is copied into the repository; only signatures, offsets and behaviors are recorded here.

## `garrysmod/bin/win64/lua_shared.dll`

### Exports used
- `CreateInterface`: the classic Source interface registry (linked list name → factory). `"LUASHARED003"` returns the `CLuaShared` singleton.
- Full LuaJIT C API: `lua_*`, `luaL_*`, `luaJIT_*` (including `lua_sethook`, `luaL_traceback`, `luaL_loadbuffer`, `lua_pcall`).
  The module binds to these symbols dynamically: no import library is needed.

### `ILuaShared` (vtable of `CLuaShared`, found via RTTI `.?AVCLuaShared@@`)
19 entries, in the order of `garrysmod_common/include/GarrysMod/Lua/LuaShared.h` (the MSVC virtual destructor takes 1 slot).
- slot 4 `CreateLuaInterface(realm, bool)`: creates the interface and stores it in `this + 0x78 + realm*8`.
- **slot 6 `GetLuaInterface(realm)`**: `return *(this + 0x78 + realm*8)` — a plain read with no side effects; returns
  `nullptr` when the realm does not exist. Realms: 0 = client, 1 = server, 2 = menu.

- **slot 9 `MountLua(pathID)`**: mounts `lua` then (unless the ID is `LuaMenu`) `gamemodes` for that Lua path ID;
  slot 10 `MountLuaAdd(dir, pathID)` adds one folder (`IFileSystem::AddSearchPath(full, id, 5)`, i.e. the tail of
  GMod's LUA priority group); slot 11 `UnMountLua(pathID)` removes every path of the ID; slot 17 `EmptyCache()`
  (position in the header; used by the Lua mount).

### Lua mount: search paths

`bin/win64/filesystem_stdio.dll`, `CreateInterface("VFileSystem022")`: one singleton (the factory returns a static
object; `VBaseFileSystem011` is the same class), and it is the object the engine uses: `lua_shared` gets its
`g_pFullFileSystem` from the same factory name. The primary vtable is `IAppSystem` then the SDK 2013 `IFileSystem` order
(checked by body). **`IAppSystem` has 5 or 9 slots depending on the build**, so the module reads the layout from the code
(`lua_mount::fs_slot_base`, coretests `test_fs_vtable_layout`) and refuses the mount when it matches neither:

| `IFileSystem` function | 5-slot `IAppSystem` (older x64 builds) | 9-slot `IAppSystem` (x86-64 build of 2026-11-19) |
|---|---|---|
| `IAppSystem` extras | — | 5 `GetDependencies` (`xor eax,eax; ret`), 6 `GetTier` (`mov eax,4; ret`), 7 `Reconnect` (`ret 0`), 8 `IsSingleton` (`mov al,1; ret`) |
| `IsSteam`, `MountSteamContent` | 5, 6 (`xor al,al; ret`, `xor eax,eax; ret`) | 9, 10 |
| **`AddSearchPath(path, pathID, addType)`** (re-adding a path+ID moves it, no duplicate) | **7** | **11** |
| **`RemoveSearchPath(path, pathID)`** | **8** | **12** |
| `RemoveAllSearchPaths()` | 9 | **13** |
| `RemoveSearchPaths(pathID)` / `MarkPathIDByRequestOnly(pathID, bool)` | 10 / 11 | 14 / 15 |
| `RelativePathToFullPath` / `GetSearchPath(pathID, bGetPackFiles, dest, maxLen)` | 12 / 13 | 16 / 17 |

The detection rule: slots 7 and 8 both have a body → 5; both only return a constant and 11/12 have a body → 9;
anything else → refused (`interface_missing` with a message). `lua_mount` reports it as `fs_first_slot`.

How the 9-slot layout was found (2026-11-19, `filesystem_stdio.dll` sha256 `ed28870d…`, `lua_shared.dll` `56fa0a52…`):
the module's diagnosis printed the live vptr at RVA `0xaf6e0`, not the `0xb2688` of the Ghidra project then open, which
turned out to be an analysis of the PREVIOUS file (sha256 `bd03c74f…`, same path: the per-path Ghidra project was
reused after the update). In the current file: slot 11 → `0x180012dc0` → the body that adds to the search path list
(`.bsp`/`.vpk` special cases, priority groups); slot 12 the remover; slot 13 `0x180021e30` empties the whole list.
`lua_shared`'s `MountLuaAdd` calls `[vtbl+0x58]` (11) with `(full, id, 5 | 0x100 for workshop)`, `[+0x80]` (16)
`RelativePathToFullPath`, `[+0xb0]` (22) `IsDirectory`, then `[+0x78]` (15) `(id, 1)`; `UnMountLua` calls `[+0x70]`
(14). Live (O-19 in TESTS.md): with slots 7/8 the engine's `path` output was identical before and after `lua_mount`
(they are `Reconnect` and `IsSingleton`), with 11/12 the folder appears and its autorun runs in both realms.

**GetSearchPath is not called**: on the 2026-11-19 build the old fixed slot 13 was `RemoveAllSearchPaths`. The one
call the module made (removed in 4e42f6a) wiped every search path; the next load of a new model (`SetModel` of a prop
not loaded yet) then blocked the main thread for good. The base Lua check runs `file.Exists("includes/init.lua",
"LUA")` in the server realm instead. `PrintSearchPaths` (the `path` console command) was slot 63 on the 5-slot
layout (not re-checked on the 9-slot one; the module does not call it).

GMod turned `addType` into priority groups: `(group << 1) | tail`, group 0 counted as 1; `0` puts the path before every
other one. The `path` command prints the groups by name: ENGINECORE (where addType 0 lands), LUA, MAP, ADDONCONTENT,
GMCONTENT, GMODCORE (garrysmod's own `MOD`), SOURCESDK, GAMECONTENT, MOUNTCFG (where addType 23 lands), DOWNLOADS,
FALLBACKS (`fallbacks.vpk`, also `MOD`).

- Lua path IDs: `lsv` (server realm), `lcl` (client), `LuaMenu` (menu).
- At every map start, `server.dll` calls `UnMountLua("lsv")`, then `MountLuaAdd` for each addon's `lua`/`gamemodes`,
  then `MountLua("lsv")`, then creates the server Lua state (autorun runs after this).
- `lua_shared` loads a file only if its full path maps back to a `MOD`-relative name (`FullPathToRelativePathEx(full,
  "MOD")`), and caches it under that name with its file time.

So the mounted folder adds its root near the end of `MOD` (addType 23: MOUNTCFG, after garrysmod's own paths), its `lua`/`gamemodes` to
`lsv` and `lcl` (head), and hooks
`ILuaShared` slot 9 (vtable entry patched in this process only) to put them back in front right after every
`MountLua`. The root must NOT be at the head of `MOD`: `MountLuaAdd("lua")` resolves the `lua` folder through `MOD`,
so a root at the head made the engine mount that folder's `lua/` instead of `garrysmod/lua` (seen live 2026-11-14:
no base Lua, Lua errors everywhere, black sky). It is not added to `GAME` either. Not analysed: how `client.dll` mounts `lcl` (live check), the Linux layouts.

**What the engine accepts but does not read** (chaos testing 2026-11-20, issue #9). `AddSearchPath` takes any string and
the module's records cannot tell whether the engine can open files under it, so the mount verdict comes from the engine: each
realm's `file.Exists(name, "LUA")` and `file.Time(name, "LUA")` (the module's `mount_probe` runtime op) for a few of the
folder's files, compared with the folder's file time (`lua_mount::probes_verdict`). Seen live: a UNC path (also
`\\localhost\D$\...`), an extended-length path (`\\?\D:\...`) and a root of about 236 characters (a file under it passes
MAX_PATH 260) were all accepted into `lsv`/`lcl`/`MOD` and loaded nothing; they are refused up front (`normalize_root`,
`refuse_too_long`). A folder with `lua/includes/init.lua` makes `file.Exists("includes/init.lua", "LUA")` true through the
shadow, so the base-Lua check stays true while the client hangs in its load: refused by comparing file names with the game's own
`lua/` and `gamemodes/base` (`shadowed_base_files`). The `MountLua` hook's folder scan is paid on the main thread inside the load:
one scan serves the server's and the client's hook of a load (`record_load_start`), and a missing folder or `lua/` records nothing.


### `CLuaInterface` (implements `ILuaInterface : ILuaBase`)
- 0x9B0-byte allocation; vptr at +0x0.
- **`lua_State*` at +0x8**: vtable slot 0 (`ILuaBase::Top`) is `mov rcx,[rcx+8]; jmp lua_gettop`.
- Conversely, `lua_State` holds at **+0x78** a pointer to its `ILuaBase` (92+22-byte header, see `struct lua_State` in
  garrysmod_common, aligned to 8).
- The module **validates** both offsets on every access (`L->luabase == iface`, guarded reads). If a future build changes
  the layout, the realm is reported unavailable instead of crashing.

## `bin/win64/tier0.dll`

Relevant exports:
- `SpewOutputFunc(SpewOutputFunc_t)` / `GetSpewOutputFunc()`: the Source 2013 spew mechanism (`SpewRetval_t (*)(SpewType_t, const char *)`). **Gone from the Windows x86-64 tier0 of 2026-11-19** (sha256 ca36907c...): console capture then uses the logging listener (`LoggingSystem_RegisterLoggingListener`, slot 0 `Log(ctx, msg)`, ctx = {channel, flags, severity, color}; checked on that build, and the capture works live).
  Used to capture the console by chaining the previous function.
- `GetSpewOutputColor()`, `GetSpewOutputLevel()`, `GetSpewOutputGroup()`.
- `GMOD_SpewBufferStart/End/Flush` (GMod-specific, unused).
- `Plat_GetCommandLine` (`const char *()`) and `CommandLine_Tier0` (`ICommandLine *`). The module uses `Plat_GetCommandLine`:
  this avoids depending on the order of overloads in the `ICommandLine` vtable.
- `Msg`, `Warning`, `ConMsg`, `ConColorMsg`, `Plat_FloatTime`.

## Lua console commands (string search)
`client.dll`, `server.dll` and `menusystem.dll` (x64) contain `lua_run`, `lua_run_cl`, `lua_openscript` and `lua_openscript_cl`,
but **neither `lua_run_menu` nor `lua_openscript_menu`**. The menu state therefore cannot be bootstrapped from the command line.

## `garrysmod/bin/win64/client.dll` — server address (network lock)

- `game.GetIPAddress` (client) is implemented as follows: `engineClient->GetNetChannelInfo()` (**slot 72, offset 0x240** of the
  `VEngineClient015` vtable), then, if non-null, `INetChannelInfo::GetAddress()` (**slot 1**). Without a net channel, it returns `"0.0.0.0:0"`.
- The engine pointer is obtained through `CreateInterface("VEngineClient015")` from `engine.dll` (standard factory, verified during
  `client.dll` initialization).
- These indices match the public order of `cdll_int.h` and `inetchannelinfo.h` (Source SDK 2013).
- The module runs **no code in the client realm** to read the address. Any value that is not `a.b.c.d:port` or
  `loopback` is treated as unknown, and the client realm is then locked.

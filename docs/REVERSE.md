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

### `CLuaInterface` (implements `ILuaInterface : ILuaBase`)
- 0x9B0-byte allocation; vptr at +0x0.
- **`lua_State*` at +0x8**: vtable slot 0 (`ILuaBase::Top`) is `mov rcx,[rcx+8]; jmp lua_gettop`.
- Conversely, `lua_State` holds at **+0x78** a pointer to its `ILuaBase` (92+22-byte header, see `struct lua_State` in
  garrysmod_common, aligned to 8).
- The module **validates** both offsets on every access (`L->luabase == iface`, guarded reads). If a future build changes
  the layout, the realm is reported unavailable instead of crashing.

## `bin/win64/tier0.dll`

Relevant exports:
- `SpewOutputFunc(SpewOutputFunc_t)` / `GetSpewOutputFunc()`: the Source 2013 spew mechanism (`SpewRetval_t (*)(SpewType_t, const char *)`).
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

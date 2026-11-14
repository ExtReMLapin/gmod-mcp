// Per-instance Lua overlay: a folder (e.g. a git worktree of an addon) searched before every addon for Lua files, in
// this game process only. The engine's own search paths are used, so include(), autorun and AddCSLuaFile all see it.
//
// How (see docs/REVERSE.md, "Lua overlay"): IFileSystem (VFileSystem022) AddSearchPath of the root at the END of "MOD"
// (lua_shared only loads files that resolve to a MOD-relative name; at the head, the engine would mount the root's lua/
// instead of garrysmod/lua) and of <root>/lua (and gamemodes/) at the HEAD of the Lua path IDs "lsv"/"lcl". The server
// unmounts "lsv" at every map start and mounts it again through ILuaShared::MountLua: that call is hooked (vtable
// slot, this process only) to put the folder back in front each time. gmod-mcp's own path lookups (tools_core.cpp
// lua_candidates, find_game_lua, chunk_for) follow the same order and names. Windows game clients only: the Linux
// vtable layouts are not verified, and a dedicated server's filesystem is not reachable the same way.
#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace overlay {

// Pure: checks a user-given folder and returns the overlay root (the folder that contains lua/). Accepts the addon
// root or its lua/ folder. Refuses relative paths, missing folders, a root without lua/, and characters that would
// break a command line or the engine's ';'-separated path lists (quotes, ';', line breaks).
bool normalize_root(const std::string &in, std::string &root, std::string &err);

// Main thread. root empty = remove the overlay. Returns {"ok":true,"result":...} or an agent error.
nlohmann::json set(const std::string &root, bool empty_cache);
// Main thread: the current overlay and the Lua search paths the engine uses now.
nlohmann::json info();
// Current overlay root ("" if none). Any thread.
std::string current();
// Tests only: sets the current root without touching the engine.
void set_current_for_tests(const std::string &root);
// False on platforms where the engine layouts are not verified (why says so).
bool supported(std::string *why);

}  // namespace overlay

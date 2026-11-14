// Per-instance Lua overlay: a folder (e.g. a git worktree of an addon) searched before every addon for Lua files, in
// this game process only. The engine's own search paths are used, so include(), autorun and AddCSLuaFile all see it.
//
// How (see docs/REVERSE.md, "Lua overlay"): IFileSystem (VFileSystem022) AddSearchPath at the head of "MOD"/"GAME"
// (lua_shared only loads files that resolve to a MOD-relative name) and of the Lua path IDs "lsv"/"lcl". The server
// unmounts "lsv" at every map start and mounts it again through ILuaShared::MountLua: that call is hooked (vtable
// slot, this process only) to put the overlay back in front each time. Windows only for now: the Linux vtable layouts
// of IFileSystem are not verified.
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
// False on platforms where the engine layouts are not verified (why says so).
bool supported(std::string *why);

}  // namespace overlay

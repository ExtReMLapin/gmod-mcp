// Per-instance Lua mount: a folder (e.g. a git worktree of an addon) searched before every addon for Lua files, in
// this game process only. The engine's own search paths are used, so include(), autorun and AddCSLuaFile all see it.
//
// How (see docs/REVERSE.md, "Lua mount"): IFileSystem (VFileSystem022) AddSearchPath of the root at the END of "MOD"
// (lua_shared only loads files that resolve to a MOD-relative name; at the head, the engine would mount the root's lua/
// instead of garrysmod/lua) and of <root>/lua (and gamemodes/) at the HEAD of the Lua path IDs "lsv"/"lcl". The server
// unmounts "lsv" at every map start and mounts it again through ILuaShared::MountLua: that call is hooked (vtable
// slot, this process only) to put the folder back in front each time. gmod-mcp's own path lookups (tools_core.cpp
// lua_candidates, find_game_lua, chunk_for) follow the same order and names. Windows game clients only: the Linux
// vtable layouts are not verified, and a dedicated server's filesystem is not reachable the same way.
#pragma once

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lua_mount {

// Pure: checks a user-given folder and returns the mount root (the folder that contains lua/). Accepts the addon
// root or its lua/ folder. Refuses relative paths, missing folders, a root without lua/, and characters that would
// break a command line or the engine's ';'-separated path lists (quotes, ';', line breaks).
bool normalize_root(const std::string &in, std::string &root, std::string &err);
// Pure: "" if `root` may be mounted for the game in `game_dir` (the garrysmod folder), else why not. A folder inside
// the game folder (garrysmod itself, an installed addon) is already searched by the engine: mounting it would only
// reorder the game's own paths.
std::string refuse_inside_game(const std::string &root, const std::string &game_dir);

// Main thread. root empty = remove the mounted folder. Returns {"ok":true,"result":...} or an agent error.
nlohmann::json set(const std::string &root, bool empty_cache);
// Main thread: the current mounted folder and the Lua search paths the engine uses now.
nlohmann::json info();
// Current mount root ("" if none). Any thread.
std::string current();
// Tests only: sets the current root without touching the engine.
void set_current_for_tests(const std::string &root);
// False on platforms where the engine layouts are not verified (why says so).
bool supported(std::string *why);
// Main thread. Empties the engine's Lua file cache (a mounted file edited behind its back may be served stale from it);
// false when there is no engine to ask (unsupported platform, interface missing).
bool empty_cache();

// ---- What each realm loaded from the mounted folder (Issue #9) ----
// The engine, not this module, resolves every include()/autorun/AddCSLuaFile against the search paths, so the module
// cannot see each file load. It records what it can know for certain: when the MountLua hook put the folder in front
// for a realm (right before that realm's Lua loads), the folder's files and their times at that moment. A file of the
// snapshot is what the realm loaded from the mount; a file that is no longer what the snapshot says changed since.
struct FileStamp {
    std::string rel;       // root-relative, '/' separators, as the engine names it: "lua/pba/x.lua", "gamemodes/g/init.lua"
    long long mtime = 0;   // file time (ticks of the file system clock); -1 never stored
};
using Files = std::map<std::string, FileStamp>;  // key: lower-case rel
// Every .lua under <root>/lua and <root>/gamemodes.
Files scan_files(const std::string &root);
// File time in ticks of the file system clock, -1 if the file cannot be read.
long long mtime_of(const std::string &abs);
// "lua/x.lua" if `abs` is inside the current mount root, else "".
std::string rel_in_mount(const std::string &abs);

// The folder was put in front of the realm's Lua path (hook, main thread): `files` is the folder at that moment. The
// record becomes the realm's once its runtime is injected (a new Lua state is a new load of every file).
void note_load_start(int realm, const std::string &root, Files files);
// The realm's runtime was injected: its Lua state exists. A pending load start becomes its record; without one the
// realm loaded without the mounted folder.
void note_realm_loaded(int realm);
// A file of the mounted folder was run again in the realm (lua_reload): from then on it counts as loaded from the
// mount, with the file time seen before it was read.
void note_reloaded(int realm, const std::string &root, const std::string &rel, long long mtime);
// The realm loaded its Lua with the CURRENT mounted folder in front.
bool realm_loaded_with_mount(int realm);
// Did the realm get this file ("lua/pba/x.lua") from the mounted folder (in the load-time snapshot, or reloaded since)?
// `reloaded` tells which of the two.
bool loaded_from_mount(int realm, const std::string &rel, bool *reloaded = nullptr);
struct Change {
    std::string rel;
    std::string reason;  // "modified" (time differs, newer or older), "added" (not there at load), "deleted"
};
// Files of the mounted folder that are not what the realm loaded (empty if the realm has no record).
std::vector<Change> changed_files(int realm);
// Tests only: forget every record.
void forget_loads_for_tests();

}  // namespace lua_mount

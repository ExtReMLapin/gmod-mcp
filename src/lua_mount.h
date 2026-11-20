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

#include <cstdint>
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

// Pure: "" if every .lua file under <root>/lua and <root>/gamemodes has a full path (root, separator, root-relative name)
// of at most `max_path` characters (Windows MAX_PATH leaves 259), else why not. The engine accepts a root whose files pass
// it into its search paths but cannot open any of them: the mount would report active and load nothing.
std::string refuse_too_long(const std::string &root, size_t max_path = 259);
// Pure: the files of the folder that would shadow the game's own base Lua (named from the mount root: "lua/includes/init.lua"):
// every .lua under <root>/lua that also exists under <game_dir>/lua, and under <root>/gamemodes/base that exists in the game's
// own gamemodes/base. A folder with lua/includes/init.lua replaced the game's base Lua and the client hung in its load.
// Sorted; empty when `game_dir` is "" (unknown).
std::vector<std::string> shadowed_base_files(const std::string &root, const std::string &game_dir);
// Pure: the one check run before a folder is mounted (lua_mount tool, instances_launch, -mcp_lua_mount): "" if it may be,
// else why not, with the error `code` ("invalid_params", or "shadows_base_lua"). `game_dir` is the garrysmod folder
// ("" = unknown: only the other checks run).
std::string refuse_mount(const std::string &root, const std::string &game_dir, bool allow_shadow, std::string *code = nullptr);

// Pure: whether `code` (the first bytes of a function) only returns a constant: `ret`, `ret n`, or one
// `xor`/`mov` into al/eax/rax then `ret`. A jump thunk is not followed (not constant).
bool is_constant_return(const std::vector<uint8_t> &code);
// Pure: first IFileSystem slot of the "VFileSystem022" primary vtable, from the code of slots 7, 8, 11 and 12:
// 5 (IAppSystem with 5 slots: 7 AddSearchPath, 8 RemoveSearchPath), 9 (IAppSystem with 9 slots, GMod x86-64 build of
// 2026-11-19: 7 Reconnect and 8 IsSingleton only return a constant; 11 AddSearchPath, 12 RemoveSearchPath), or -1 if
// the code matches neither (the mount is then refused: a wrong slot can wipe every search path). docs/REVERSE.md.
int fs_slot_base(const std::vector<uint8_t> &s7, const std::vector<uint8_t> &s8, const std::vector<uint8_t> &s11,
                 const std::vector<uint8_t> &s12);

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
// How many times scan_files ran (a scan stats every file of the folder: tests count them).
uint64_t scan_count();
// File time in ticks of the file system clock, -1 if the file cannot be read.
long long mtime_of(const std::string &abs);
// "lua/x.lua" if `abs` is inside the current mount root, else "".
std::string rel_in_mount(const std::string &abs);

// Whether the mount root and its lua/ folder exist (a folder renamed or deleted since it was mounted is not served).
bool folder_present(const std::string &root);
// The hook's entry (main thread): the folder was put in front of the realm's Lua path. Records the folder's files, unless
// the folder (or its lua/) is gone: the realm then loads without it and must not be reported as loaded with the mount. The
// server's and the client's hook of one map load (seconds apart at most) share one scan of the folder.
void record_load_start(int realm, const std::string &root);
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
// The folder the realm loaded this file from when that is NOT the mounted folder any more (another folder was mounted, or
// none, without a map load since): its root, else "". The realm still runs that copy.
std::string loaded_from_other_mount(int realm, const std::string &rel);
struct Change {
    std::string rel;
    std::string reason;  // "modified" (time differs, newer or older), "added" (not there at load), "deleted"
};
// Files of the mounted folder that are not what the realm loaded (empty if the realm has no record).
std::vector<Change> changed_files(int realm);
// Whether the realm has a record (a load, or files run again by lua_reload) for the CURRENT mounted folder.
bool has_record(int realm);
// Same, against a scan the caller already made (`now`): several realms share one scan.
std::vector<Change> changed_files(int realm, const Files &now);
// ---- Does the engine really serve the folder? (Issue #9, chaos testing 2026-11-20) ----
// The bookkeeping above is the module's own: a path the engine accepts but cannot read (UNC, junction, too long...) is
// recorded as loaded and loads nothing. So each realm's file system is asked for a few files of the folder.
struct Probe {
    std::string name;         // the Lua path as file.Exists takes it: "pba/x.lua" (no lua/ or gamemodes/ prefix)
    long long unix_time = -1; // the folder file's time in Unix seconds as file.Time answers, -1 unknown
};
// A few files to ask about, spread over the snapshot, lua/ files first (a gamemodes/ file only when there are too few).
std::vector<Probe> pick_probes(const Files &files, size_t n = 3);
// File system clock ticks (mtime_of) to Unix seconds; -1 stays -1.
long long unix_seconds_of(long long ticks);
// The realm's answers ([{exists, time?}], one per probe) as a verdict: true when some probe is served from the folder (it
// exists and its time is the folder file's, give or take 2 s; existence alone when the time is unknown), false when none
// is, null when there is no usable answer.
nlohmann::json probes_verdict(const std::vector<Probe> &probes, const nlohmann::json &answers);
// The files to probe for a realm: its load-time snapshot, else the folder as it is now.
Files probe_sample(int realm);

// Tests only: forget every record.
void forget_loads_for_tests();

}  // namespace lua_mount

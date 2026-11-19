#include "lua_mount.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <mutex>

#include <GarrysMod/Lua/LuaShared.h>

#include "agent.h"
#include "log.h"
#include "luaapi.h"
#include "platform/platform.h"
#include "realms.h"
#include "util.h"

namespace lua_mount {

using nlohmann::json;
namespace fs = std::filesystem;

static std::mutex g_mu;
static std::string g_root;

std::string current() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_root;
}

void set_current_for_tests(const std::string &root) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_root = root;
}

#ifdef _WIN32

// IFileSystem ("VFileSystem022", filesystem_stdio) primary vtable slots, verified in the x64 Windows binary
// (docs/REVERSE.md): AddSearchPath 7, RemoveSearchPath 8, GetSearchPath 13. GMod's addType: 0 = before every path.
constexpr int kAddSearchPath = 7, kRemoveSearchPath = 8;  // GetSearchPath (13): not called, see base_lua_found
// ILuaShared (LUASHARED003) slot of MountLua in garrysmod_common's LuaShared.h, MSVC layout (one destructor slot).
constexpr int kMountLua = 9;

using CreateInterfaceFn = void *(*)(const char *, int *);
using AddFn = void (*)(void *, const char *, const char *, int);
using RemoveFn = bool (*)(void *, const char *, const char *);
using MountLuaFn = void (*)(void *, const char *);

static void *g_fs = nullptr;
static GarrysMod::Lua::ILuaShared *g_ls = nullptr;
static void **g_mount_slot = nullptr;
static MountLuaFn g_orig_mount = nullptr;
static int g_in_hook = 0;
static uint64_t g_hook_calls = 0;
static json g_hook_ids = json::object();  // path IDs seen by the hook, with counts (diagnosis)

static void *get_iface(const std::vector<std::string> &names, const char *version) {
    void *mod = plat::find_loaded_module(names);
    if (!mod) return nullptr;
    auto ci = reinterpret_cast<CreateInterfaceFn>(plat::find_symbol(mod, "CreateInterface"));
    if (!ci) return nullptr;
    int rc = 0;
    return ci(version, &rc);
}

static bool ensure(std::string &err) {
    if (!g_fs) g_fs = get_iface(plat::filesystem_names(), "VFileSystem022");
    if (!g_ls) g_ls = static_cast<GarrysMod::Lua::ILuaShared *>(get_iface(plat::lua_shared_names(), GMOD_LUASHARED_INTERFACE));
    if (!g_fs)
        err = agent::config().mode == agent::Mode::Dedicated
                  ? "the Lua mount is not supported on a dedicated server yet (its filesystem interface is not found); use "
                    "it on a game client (instances_launch {lua_mount})"
                  : "IFileSystem (VFileSystem022) not found in filesystem_stdio";
    else if (!g_ls) err = "ILuaShared (" GMOD_LUASHARED_INTERFACE ") not found in lua_shared";
    return g_fs && g_ls;
}

static void **vtable(void *obj) { return *static_cast<void ***>(obj); }

// GMod's addType = (group << 1) | tail bit, groups 0..11 (docs/REVERSE.md). 0 = before every path; 23 = after every path.
constexpr int kHead = 0, kTail = (11 << 1) | 1;
static void fs_add(const std::string &path, const char *id, int add_type = kHead) {
    reinterpret_cast<AddFn>(vtable(g_fs)[kAddSearchPath])(g_fs, path.c_str(), id, add_type);
}
static bool fs_remove(const std::string &path, const char *id) {
    return reinterpret_cast<RemoveFn>(vtable(g_fs)[kRemoveSearchPath])(g_fs, path.c_str(), id);
}
// IFileSystem::GetSearchPath (slot 13) is NOT called any more: on the x86-64 build of 2026-11-19 one call
// from the module (lua_mount {} or the end of a lua_mount {path}) made the next load of a new model (SetModel of a prop
// not loaded yet) block the main thread for good, and it returned no path for "lsv"/"lcl" anyway. The engine's own
// `path` command does not do that. See docs/REVERSE.md. The base Lua is checked through the server realm instead.

// Whether the server realm finds the base Lua (includes/init.lua through its LUA paths): true/false, or null when there
// is no server realm (main menu) or the check could not run. Main thread only.
static json base_lua_found() {
    lua_State *L = realms::state(realms::SERVER);
    if (!L) return nullptr;
    std::string out, err;
    if (!lua::run(L, "return file.Exists('includes/init.lua', 'LUA') and 'yes' or 'no'", "=mcpagent:base_lua", &out, err))
        return nullptr;
    if (out == "yes") return true;
    if (out == "no") return false;
    return nullptr;
}

static bool is_dir(const std::string &p) {
    std::error_code ec;
    return fs::is_directory(fs::u8path(p), ec);
}

// The mounted folder's Lua folders, at the head of one Lua path ID ("lsv" server, "lcl" client).
static void add_lua(const std::string &root, const char *id) {
    fs_add(root + "\\lua", id);
    if (is_dir(root + "\\gamemodes")) fs_add(root + "\\gamemodes", id);
}

// ILuaShared::MountLua is what the server (at every map start, after unmounting "lsv") and the client call to mount
// a Lua path ID: put the mounted folder back in front right after it.
static void hooked_mount(void *self, const char *id) {
    g_orig_mount(self, id);
    if (g_in_hook || !id) return;
    ++g_in_hook;
    ++g_hook_calls;
    g_hook_ids[id] = g_hook_ids.value(id, 0) + 1;
    std::string root = current();
    bool sv = std::strcmp(id, "lsv") == 0, cl = std::strcmp(id, "lcl") == 0;
    if (!root.empty() && (sv || cl)) {
        add_lua(root, id);
        // Issue #9: the folder is in front for the Lua this realm loads next: remember what it holds right now.
        try {
            note_load_start(sv ? realms::SERVER : realms::CLIENT, root, scan_files(root));
        } catch (...) {
        }
    }
    --g_in_hook;
}

static bool hook(bool on, std::string &err) {
    void **vt = vtable(g_ls);
    void **slot = &vt[kMountLua];
    void *cur = nullptr;
    if (!plat::safe_read(slot, &cur, sizeof cur) || !cur) {
        err = "ILuaShared vtable unreadable";
        return false;
    }
    if (on) {
        if (cur == reinterpret_cast<void *>(&hooked_mount)) return true;
        g_orig_mount = reinterpret_cast<MountLuaFn>(cur);
        g_mount_slot = slot;
        if (!plat::patch_pointer(slot, reinterpret_cast<void *>(&hooked_mount))) {
            err = "could not patch ILuaShared::MountLua";
            return false;
        }
        return true;
    }
    if (cur != reinterpret_cast<void *>(&hooked_mount) || !g_orig_mount) return true;
    if (!plat::patch_pointer(slot, reinterpret_cast<void *>(g_orig_mount))) {
        err = "could not restore ILuaShared::MountLua";
        return false;
    }
    return true;
}

bool supported(std::string *) { return true; }

bool empty_cache() {
    std::string err;
    if (!ensure(err)) return false;
    g_ls->EmptyCache();
    return true;
}

json info() {
    std::string err;
    if (!ensure(err)) return json{{"ok", false}, {"error", {{"code", "interface_missing"}, {"message", err}}}};
    std::string root = current();
    void *cur = nullptr;
    plat::safe_read(&vtable(g_ls)[kMountLua], &cur, sizeof cur);
    // null at the main menu (no server realm yet): only meaningful once a map is loaded.
    json base_ok = base_lua_found();
    json r{{"lua_mount", root.empty() ? json(nullptr) : json(root)},
           {"base_lua_mounted", base_ok},
           {"mount_hook", cur == reinterpret_cast<void *>(&hooked_mount)},
           {"hook_calls", g_hook_calls},
           {"hook_path_ids", g_hook_ids}};
    if (base_ok == json(false))
        r["warning"] = "the game's own lua/ folder is missing from the server Lua paths: the game cannot run its base Lua. "
                       "Remove the mounted folder (lua_mount {path: null}) and close this client.";
    return json{{"ok", true}, {"result", r}};
}

json set(const std::string &root, bool empty_cache) {
    std::string err;
    if (!ensure(err)) return json{{"ok", false}, {"error", {{"code", "interface_missing"}, {"message", err}}}};
    std::string old = current();
    json removed = json::object();
    if (!old.empty()) {
        removed["MOD"] = fs_remove(old, "MOD");
        for (const char *id : {"lsv", "lcl"}) {
            removed[std::string(id) + ":lua"] = fs_remove(old + "\\lua", id);
            if (is_dir(old + "\\gamemodes")) removed[std::string(id) + ":gamemodes"] = fs_remove(old + "\\gamemodes", id);
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_root = root;
    }
    if (!root.empty()) {
        // lua_shared only loads a file whose full path maps back to a MOD-relative name: the root must be a MOD path.
        // At the END of MOD: the engine finds the base "lua" and "gamemodes" folders through MOD (MountLuaAdd), and an
        // a root at the head made it mount that folder's lua/ INSTEAD of garrysmod/lua (seen live 2026-11-14: no
        // base Lua, errors everywhere, black sky). Not added to GAME either: only Lua is overlaid.
        fs_add(root, "MOD", kTail);
        add_lua(root, "lsv");
        add_lua(root, "lcl");
        if (!hook(true, err)) return json{{"ok", false}, {"error", {{"code", "hook_failed"}, {"message", err}}}};
    } else if (!hook(false, err)) {
        return json{{"ok", false}, {"error", {{"code", "hook_failed"}, {"message", err}}}};
    }
    // Cached copies are keyed by MOD-relative name and file time: an addon file and its mounted copy share the name.
    if (empty_cache) g_ls->EmptyCache();
    logx::info("Lua mount %s%s", root.empty() ? "removed" : "set: ", root.c_str());
    json r = info();
    if (!r.value("ok", false)) return r;
    json &res = r["result"];
    if (!old.empty()) {
        res["previous"] = old;
        res["removed"] = removed;
    }
    res["cache_emptied"] = empty_cache;
    res["note"] = root.empty()
                      ? "mounted folder removed: the next map load uses the addons' own files again (code already loaded stays until then)"
                      : "the mounted folder applies to files loaded from now on: include(), autorun and AddCSLuaFile of the NEXT map "
                        "load read it first. Code already running is not re-run: call map_load now.";
    return r;
}

#else

bool supported(std::string *why) {
    if (why)
        *why = "the Lua mount is Windows only for now (the engine's filesystem interface layout is not verified on this "
               "platform)";
    return false;
}

json info() {
    std::string why;
    supported(&why);
    return json{{"ok", false}, {"error", {{"code", "unsupported_platform"}, {"message", why}}}};
}

json set(const std::string &, bool) { return info(); }

bool empty_cache() { return false; }

#endif

}  // namespace lua_mount

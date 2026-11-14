#include "overlay.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <mutex>

#include <GarrysMod/Lua/LuaShared.h>

#include "agent.h"
#include "log.h"
#include "platform/platform.h"
#include "util.h"

namespace overlay {

using nlohmann::json;
namespace fs = std::filesystem;

static std::mutex g_mu;
static std::string g_root;

std::string current() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_root;
}

#ifdef _WIN32

// IFileSystem ("VFileSystem022", filesystem_stdio) primary vtable slots, verified in the x64 Windows binary
// (docs/REVERSE.md): AddSearchPath 7, RemoveSearchPath 8, GetSearchPath 13. GMod's addType: 0 = before every path.
constexpr int kAddSearchPath = 7, kRemoveSearchPath = 8, kGetSearchPath = 13;
// ILuaShared (LUASHARED003) slot of MountLua in garrysmod_common's LuaShared.h, MSVC layout (one destructor slot).
constexpr int kMountLua = 9;

using CreateInterfaceFn = void *(*)(const char *, int *);
using AddFn = void (*)(void *, const char *, const char *, int);
using RemoveFn = bool (*)(void *, const char *, const char *);
using GetFn = int (*)(void *, const char *, bool, char *, int);
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
                  ? "the Lua overlay is not supported on a dedicated server yet (its filesystem interface is not found); use "
                    "it on a game client (instances_launch {overlay})"
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
static json fs_paths(const char *id) {
    static char buf[32768];
    int n = reinterpret_cast<GetFn>(vtable(g_fs)[kGetSearchPath])(g_fs, id, false, buf, (int)sizeof buf);
    json out = json::array();
    if (n <= 0) return out;
    buf[sizeof buf - 1] = 0;
    std::string all(buf);
    size_t start = 0;
    while (start <= all.size()) {
        size_t end = all.find(';', start);
        if (end == std::string::npos) end = all.size();
        if (end > start) out.push_back(all.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

static bool is_dir(const std::string &p) {
    std::error_code ec;
    return fs::is_directory(fs::u8path(p), ec);
}

// The overlay's Lua folders, at the head of one Lua path ID ("lsv" server, "lcl" client).
static void add_lua(const std::string &root, const char *id) {
    fs_add(root + "\\lua", id);
    if (is_dir(root + "\\gamemodes")) fs_add(root + "\\gamemodes", id);
}

// ILuaShared::MountLua is what the server (at every map start, after unmounting "lsv") and the client call to mount
// a Lua path ID: put the overlay back in front right after it.
static void hooked_mount(void *self, const char *id) {
    g_orig_mount(self, id);
    if (g_in_hook || !id) return;
    ++g_in_hook;
    ++g_hook_calls;
    g_hook_ids[id] = g_hook_ids.value(id, 0) + 1;
    std::string root = current();
    if (!root.empty() && (std::strcmp(id, "lsv") == 0 || std::strcmp(id, "lcl") == 0)) add_lua(root, id);
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

json info() {
    std::string err;
    if (!ensure(err)) return json{{"ok", false}, {"error", {{"code", "interface_missing"}, {"message", err}}}};
    std::string root = current();
    void *cur = nullptr;
    plat::safe_read(&vtable(g_ls)[kMountLua], &cur, sizeof cur);
    json lsv = fs_paths("lsv");
    std::string base = util::to_lower(agent::config().game_dir + "/lua/");
    std::replace(base.begin(), base.end(), '/', '\\');
    // Only meaningful once the engine has mounted the server Lua paths (a map is loaded or loading): at the main menu
    // lsv holds at most the overlay's own folders.
    std::string ov = util::to_lower(root);
    std::replace(ov.begin(), ov.end(), '/', '\\');
    bool engine_mounted = false, base_ok = false;
    for (const auto &p : lsv) {
        std::string l = util::to_lower(p.get<std::string>());
        if (ov.empty() || l.rfind(ov, 0) != 0) engine_mounted = true;
        if (l == base) base_ok = true;
    }
    if (!engine_mounted) base_ok = true;
    json r{{"overlay", root.empty() ? json(nullptr) : json(root)},
           {"base_lua_mounted", base_ok},
           {"mount_hook", cur == reinterpret_cast<void *>(&hooked_mount)},
           {"hook_calls", g_hook_calls},
           {"hook_path_ids", g_hook_ids},
           {"search_paths", {{"lsv", lsv}, {"lcl", fs_paths("lcl")}}}};
    if (!base_ok)
        r["warning"] = "the game's own lua/ folder is missing from the server Lua paths: the game cannot run its base Lua. "
                       "Remove the overlay (lua_overlay {path: null}) and close this client.";
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
        // overlay at the head made it mount the overlay's lua/ INSTEAD of garrysmod/lua (seen live 2026-11-14: no
        // base Lua, errors everywhere, black sky). Not added to GAME either: only Lua is overlaid.
        fs_add(root, "MOD", kTail);
        add_lua(root, "lsv");
        add_lua(root, "lcl");
        if (!hook(true, err)) return json{{"ok", false}, {"error", {{"code", "hook_failed"}, {"message", err}}}};
    } else if (!hook(false, err)) {
        return json{{"ok", false}, {"error", {{"code", "hook_failed"}, {"message", err}}}};
    }
    // Cached copies are keyed by MOD-relative name and file time: an addon file and its overlay copy share the name.
    if (empty_cache) g_ls->EmptyCache();
    logx::info("Lua overlay %s%s", root.empty() ? "removed" : "set: ", root.c_str());
    json r = info();
    if (!r.value("ok", false)) return r;
    json &res = r["result"];
    if (!old.empty()) {
        res["previous"] = old;
        res["removed"] = removed;
    }
    res["cache_emptied"] = empty_cache;
    res["note"] = root.empty()
                      ? "overlay removed: the next map load uses the addons' own files again (code already loaded stays until then)"
                      : "the overlay applies to files loaded from now on: include(), autorun and AddCSLuaFile of the NEXT map "
                        "load read it first. Code already running is not re-run: call map_load now.";
    return r;
}

#else

bool supported(std::string *why) {
    if (why)
        *why = "the Lua overlay is Windows only for now (the engine's filesystem interface layout is not verified on this "
               "platform)";
    return false;
}

json info() {
    std::string why;
    supported(&why);
    return json{{"ok", false}, {"error", {{"code", "unsupported_platform"}, {"message", why}}}};
}

json set(const std::string &, bool) { return info(); }

#endif

}  // namespace overlay

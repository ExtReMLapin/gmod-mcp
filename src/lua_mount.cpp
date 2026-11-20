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

bool is_constant_return(const std::vector<uint8_t> &c) {
    auto at = [&](size_t i) { return i < c.size() ? c[i] : -1; };
    size_t i = 0;
    if (at(0) == 0x48) i = 1;  // REX.W: xor rax,rax
    int op = at(i);
    if (op == 0xc3) return i == 0;                                        // ret
    if (op == 0xc2) return i == 0 && c.size() >= 3;                       // ret imm16
    if ((op == 0x30 || op == 0x32) && i == 0 && at(1) == 0xc0) return at(2) == 0xc3;  // xor al,al; ret
    if ((op == 0x31 || op == 0x33) && at(i + 1) == 0xc0) return at(i + 2) == 0xc3;    // xor eax,eax (rax); ret
    if (op == 0xb0 && i == 0) return at(2) == 0xc3;                       // mov al,imm8; ret
    if (op == 0xb8 && i == 0) return at(5) == 0xc3;                       // mov eax,imm32; ret
    return false;
}

int fs_slot_base(const std::vector<uint8_t> &s7, const std::vector<uint8_t> &s8, const std::vector<uint8_t> &s11,
                 const std::vector<uint8_t> &s12) {
    if (s7.empty() || s8.empty()) return -1;
    bool c7 = is_constant_return(s7), c8 = is_constant_return(s8);
    // 5-slot IAppSystem: 7 AddSearchPath and 8 RemoveSearchPath have a body.
    if (!c7 && !c8) return 5;
    // 9-slot IAppSystem (GetDependencies, GetTier, Reconnect, IsSingleton after Shutdown): 7 and 8 return a constant,
    // and the search path functions at 11 and 12 have a body.
    if (c7 && c8 && !s11.empty() && !s12.empty() && !is_constant_return(s11) && !is_constant_return(s12)) return 9;
    return -1;
}

#ifdef _WIN32

// IFileSystem ("VFileSystem022", filesystem_stdio) primary vtable: AddSearchPath and RemoveSearchPath are the 3rd and
// 4th IFileSystem slots, after IAppSystem's. IAppSystem has 5 slots on older builds and 9 on the x86-64 build of
// 2026-11-19, so the first IFileSystem slot is read from the code (fs_slot_base, docs/REVERSE.md): 7/8 or 11/12.
// GMod's addType: 0 = before every path.
constexpr int kAddSearchPathOfs = 2, kRemoveSearchPathOfs = 3;
// ILuaShared (LUASHARED003) slot of MountLua in garrysmod_common's LuaShared.h, MSVC layout (one destructor slot).
constexpr int kMountLua = 9;

using CreateInterfaceFn = void *(*)(const char *, int *);
using AddFn = void (*)(void *, const char *, const char *, int);
using RemoveFn = bool (*)(void *, const char *, const char *);
using MountLuaFn = void (*)(void *, const char *);

static void *g_fs = nullptr;
static int g_fs_base = 0;  // first IFileSystem slot (5 or 9), 0 until read, -1 unknown layout
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

static void **vtable(void *obj) { return *static_cast<void ***>(obj); }

// The first bytes of the function at vtable slot `i` of `obj` (empty if unreadable).
static std::vector<uint8_t> slot_code(void *obj, int i) {
    void **vt = nullptr, *fn = nullptr;
    if (!plat::safe_read(obj, &vt, sizeof vt) || !vt || !plat::safe_read(&vt[i], &fn, sizeof fn) || !fn) return {};
    std::vector<uint8_t> code(16);
    if (!plat::safe_read(fn, code.data(), code.size())) return {};
    return code;
}

static int read_fs_slot_base() {
    int base = fs_slot_base(slot_code(g_fs, 7), slot_code(g_fs, 8), slot_code(g_fs, 11), slot_code(g_fs, 12));
    logx::info("Lua mount: IFileSystem slots start at %d%s", base, base < 0 ? " (layout not recognized)" : "");
    return base;
}

// need_layout: the caller will call AddSearchPath/RemoveSearchPath (EmptyCache alone does not need the layout).
static bool ensure(std::string &err, bool need_layout = true) {
    if (!g_fs) g_fs = get_iface(plat::filesystem_names(), "VFileSystem022");
    if (!g_ls) g_ls = static_cast<GarrysMod::Lua::ILuaShared *>(get_iface(plat::lua_shared_names(), GMOD_LUASHARED_INTERFACE));
    if (!g_fs)
        err = agent::config().mode == agent::Mode::Dedicated
                  ? "the Lua mount is not supported on a dedicated server yet (its filesystem interface is not found); use "
                    "it on a game client (instances_launch {lua_mount})"
                  : "IFileSystem (VFileSystem022) not found in filesystem_stdio";
    else if (!g_ls) err = "ILuaShared (" GMOD_LUASHARED_INTERFACE ") not found in lua_shared";
    if (!g_fs || !g_ls) return false;
    if (g_fs_base == 0) g_fs_base = read_fs_slot_base();
    if (g_fs_base < 0 && need_layout) {
        err = "this game build's IFileSystem (VFileSystem022) layout is not recognized: the Lua mount is refused rather "
              "than calling an unknown slot (docs/REVERSE.md, \"Lua mount: search paths\")";
        return false;
    }
    return true;
}

// GMod's addType = (group << 1) | tail bit, groups 0..11 (docs/REVERSE.md). 0 = before every path; 23 = after every path.
constexpr int kHead = 0, kTail = (11 << 1) | 1;
static void fs_add(const std::string &path, const char *id, int add_type = kHead) {
    reinterpret_cast<AddFn>(vtable(g_fs)[g_fs_base + kAddSearchPathOfs])(g_fs, path.c_str(), id, add_type);
}
static bool fs_remove(const std::string &path, const char *id) {
    return reinterpret_cast<RemoveFn>(vtable(g_fs)[g_fs_base + kRemoveSearchPathOfs])(g_fs, path.c_str(), id);
}
// IFileSystem::GetSearchPath is not called: the base Lua is checked through the server realm instead. (On the x86-64
// build of 2026-11-19 the old fixed slot 13 was RemoveAllSearchPaths: calling it wiped every search path and the next
// load of a new model blocked the main thread for good. docs/REVERSE.md.)

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
            record_load_start(sv ? realms::SERVER : realms::CLIENT, root);
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
    if (!ensure(err, false)) return false;
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
           {"hook_path_ids", g_hook_ids},
           {"fs_first_slot", g_fs_base}};
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

#include "realms.h"

#include <GarrysMod/Lua/LuaShared.h>

#include <atomic>

#include "layout.h"
#include "log.h"
#include "platform/platform.h"

namespace realms {

using json = nlohmann::json;
using CreateInterfaceFn = void *(*)(const char *, int *);

static GarrysMod::Lua::ILuaShared *g_shared = nullptr;
// GetLuaInterface (slot 6) is called only once its code was recognized (layout.h); the offsets below are checked on every access
// (the back reference) and counted for `status` (layout_check.lua_offsets).
static bool g_get_ok = true;
static std::atomic<int> g_backref_ok{0}, g_backref_bad{0};
static std::atomic<int> g_top_shape{-1};  // ILuaBase::Top reads the lua_State from +kStateOffset: -1 not looked at yet, 0 no, 1 yes

// Offsets verified in lua_shared x64 (see docs/REVERSE.md):
// - CLuaInterface: lua_State* at +0x8 (ILuaBase::Top = lua_gettop(this->state)).
// - lua_State: ILuaBase* luabase at +0x78 (garrysmod_common header 92+22 bytes, aligned to 8).
#if defined(_M_X64) || defined(__x86_64__)
static constexpr size_t kStateOffset = 0x8;
static constexpr size_t kLuabaseOffset = 0x78;
#else
static constexpr size_t kStateOffset = 0x4;
static constexpr size_t kLuabaseOffset = 0x48;
#endif

const char *name(int realm) {
    switch (realm) {
        case CLIENT: return "client";
        case SERVER: return "server";
        case MENU: return "menu";
        default: return "?";
    }
}

int parse(const std::string &s) {
    if (s == "client") return CLIENT;
    if (s == "server") return SERVER;
    if (s == "menu") return MENU;
    return -1;
}

// The verdict of GetLuaInterface, and the provider of the offsets' verdict (what the back-reference checks counted).
static void start_checks() {
    g_get_ok = layout::iluashared(g_shared).realms_ok();
    layout::set_provider("lua_offsets", [] {
        int ok = g_backref_ok.load(), bad = g_backref_bad.load(), top = g_top_shape.load();
        json extra{{"state_offset", (int)kStateOffset}, {"luabase_offset", (int)kLuabaseOffset}, {"backref_ok", ok}, {"backref_mismatch", bad},
                   {"top_shape", top < 0 ? json(nullptr) : json(top == 1)}};
        if (ok > 0) return layout::make_entry(layout::State::Verified, "", extra);
        if (bad > 0)
            return layout::make_entry(layout::State::Refused,
                                      "lua_State's back reference at +" + std::to_string(kLuabaseOffset) + " does not point to the interface: the offsets are not those the module was built for",
                                      extra);
        return layout::make_entry(layout::State::Pending, "", extra);
    });
}

// ILuaBase::Top (vtable slot 0 of the interface) is `mov rcx,[rcx+state_offset]; jmp lua_gettop`: a second witness of the lua_State offset,
// read once from the first interface seen (informative: the back reference, checked on every access, is what gates).
static void note_top_shape(void *iface_ptr) {
    if (!layout::kChecked || g_top_shape.load() >= 0) return;
    layout::Check c = layout::lua_top_shape(layout::slot_code(iface_ptr, 0, 16), (int)kStateOffset);
    g_top_shape = c.ok ? 1 : 0;
    if (!c.ok) logx::info("layout check: lua_offsets: %s", c.why.c_str());
}

bool init(std::string &err) {
    if (g_shared) return true;
    void *mod = plat::find_loaded_module(plat::lua_shared_names());
    if (!mod) {
        err = "lua_shared not found";
        return false;
    }
    auto ci = reinterpret_cast<CreateInterfaceFn>(plat::find_symbol(mod, "CreateInterface"));
    if (!ci) {
        err = "CreateInterface missing from lua_shared";
        return false;
    }
    int rc = 0;
    g_shared = static_cast<GarrysMod::Lua::ILuaShared *>(ci(GMOD_LUASHARED_INTERFACE, &rc));
    if (!g_shared) {
        err = "CreateInterface(" GMOD_LUASHARED_INTERFACE ") failed";
        return false;
    }
    start_checks();
    return true;
}

void *iface(int realm) {
    if (!g_shared || !g_get_ok || realm < 0 || realm >= COUNT) return nullptr;
    return g_shared->GetLuaInterface(static_cast<unsigned char>(realm));
}

void verify_layout() {
    if (!g_shared) return;
    for (int r : {MENU, SERVER, CLIENT})
        if (void *i = iface(r)) {
            note_top_shape(i);
            return;
        }
}

void set_shared_for_tests(void *shared) {
    g_shared = static_cast<GarrysMod::Lua::ILuaShared *>(shared);
    g_backref_ok = 0;
    g_backref_bad = 0;
    g_top_shape = -1;
    g_get_ok = true;
    if (g_shared) start_checks();
}

lua_State *state(int realm) {
    void *i = iface(realm);
    if (!i) return nullptr;
    note_top_shape(i);
    lua_State *L = nullptr;
    if (!plat::safe_read(static_cast<char *>(i) + kStateOffset, &L, sizeof L) || !L) return nullptr;
    // Cross-check: L->luabase must point back to the interface. Otherwise the offsets are invalid: refuse.
    void *back = nullptr;
    if (!plat::safe_read(reinterpret_cast<char *>(L) + kLuabaseOffset, &back, sizeof back) || back != i) {
        if (g_backref_bad++ == 0 && g_backref_ok.load() == 0)
            logx::warn("layout check: lua_offsets: lua_State's back reference at +%d does not point to the interface (the state offset +%d or the back-reference offset is wrong for this build)",
                       (int)kLuabaseOffset, (int)kStateOffset);
        return nullptr;
    }
    ++g_backref_ok;
    return L;
}

}  // namespace realms

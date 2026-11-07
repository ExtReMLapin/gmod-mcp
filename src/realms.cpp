#include "realms.h"

#include <GarrysMod/Lua/LuaShared.h>

#include "platform/platform.h"

namespace realms {

using CreateInterfaceFn = void *(*)(const char *, int *);

static GarrysMod::Lua::ILuaShared *g_shared = nullptr;

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
    return true;
}

void *iface(int realm) {
    if (!g_shared || realm < 0 || realm >= COUNT) return nullptr;
    return g_shared->GetLuaInterface(static_cast<unsigned char>(realm));
}

lua_State *state(int realm) {
    void *i = iface(realm);
    if (!i) return nullptr;
    lua_State *L = nullptr;
    if (!plat::safe_read(static_cast<char *>(i) + kStateOffset, &L, sizeof L) || !L) return nullptr;
    // Cross-check: L->luabase must point back to the interface. Otherwise the offsets are invalid: refuse.
    void *back = nullptr;
    if (!plat::safe_read(reinterpret_cast<char *>(L) + kLuabaseOffset, &back, sizeof back) || back != i) return nullptr;
    return L;
}

}  // namespace realms

#include "luaapi.h"

#include "platform/platform.h"

namespace lua {

static Api g_api;

const Api &api() { return g_api; }

template <typename T>
static void bind(void *mod, const char *name, T &fn, std::string &missing) {
    fn = reinterpret_cast<T>(plat::find_symbol(mod, name));
    if (!fn) missing += std::string(missing.empty() ? "" : ", ") + name;
}

bool load(std::string &err) {
    if (g_api.loaded) return true;
    void *mod = plat::find_loaded_module(plat::lua_shared_names());
    if (!mod) {
        err = "lua_shared not found in the process";
        return false;
    }
    std::string m;
    bind(mod, "lua_gettop", g_api.gettop, m);
    bind(mod, "lua_settop", g_api.settop, m);
    bind(mod, "lua_pushvalue", g_api.pushvalue, m);
    bind(mod, "lua_pushnil", g_api.pushnil, m);
    bind(mod, "lua_pushnumber", g_api.pushnumber, m);
    bind(mod, "lua_pushboolean", g_api.pushboolean, m);
    bind(mod, "lua_pushlstring", g_api.pushlstring, m);
    bind(mod, "lua_pushstring", g_api.pushstring, m);
    bind(mod, "lua_pushcclosure", g_api.pushcclosure, m);
    bind(mod, "lua_pushlightuserdata", g_api.pushlightuserdata, m);
    bind(mod, "lua_getfield", g_api.getfield, m);
    bind(mod, "lua_setfield", g_api.setfield, m);
    bind(mod, "lua_createtable", g_api.createtable, m);
    bind(mod, "lua_rawget", g_api.rawget, m);
    bind(mod, "lua_rawset", g_api.rawset, m);
    bind(mod, "lua_type", g_api.type, m);
    bind(mod, "lua_tolstring", g_api.tolstring, m);
    bind(mod, "lua_tonumber", g_api.tonumber, m);
    bind(mod, "lua_toboolean", g_api.toboolean, m);
    bind(mod, "lua_touserdata", g_api.touserdata, m);
    bind(mod, "lua_pcall", g_api.pcall, m);
    bind(mod, "luaL_loadbuffer", g_api.loadbuffer, m);
    bind(mod, "lua_remove", g_api.remove, m);
    bind(mod, "lua_insert", g_api.insert, m);
    bind(mod, "lua_error", g_api.error, m);
    bind(mod, "lua_gc", g_api.gc, m);
    bind(mod, "lua_sethook", g_api.sethook, m);
    bind(mod, "luaL_traceback", g_api.traceback, m);
    if (!m.empty()) {
        g_api.missing = m;
        err = "missing Lua symbols: " + m;
        return false;
    }
    g_api.loaded = true;
    return true;
}

std::string tostring(lua_State *L, int idx) {
    size_t n = 0;
    const char *s = g_api.tolstring(L, idx, &n);
    return s ? std::string(s, n) : std::string();
}

bool run(lua_State *L, const std::string &code, const char *chunk, std::string *out, std::string &err) {
    int top = g_api.gettop(L);
    if (g_api.loadbuffer(L, code.data(), code.size(), chunk) != 0) {
        err = tostring(L, -1);
        g_api.settop(L, top);
        return false;
    }
    if (g_api.pcall(L, 0, out ? 1 : 0, 0) != 0) {
        err = tostring(L, -1);
        g_api.settop(L, top);
        return false;
    }
    if (out) *out = tostring(L, -1);
    g_api.settop(L, top);
    return true;
}

bool call_string(lua_State *L, const char *table, const char *func, const std::string &arg, std::string &out,
                 std::string &err) {
    int top = g_api.gettop(L);
    g_api.getfield(L, GLOBALSINDEX, table);
    if (g_api.type(L, -1) != TTABLE) {
        g_api.settop(L, top);
        err = std::string("global table missing: ") + table;
        return false;
    }
    g_api.getfield(L, -1, func);
    if (g_api.type(L, -1) != TFUNCTION) {
        g_api.settop(L, top);
        err = std::string("function missing: ") + table + "." + func;
        return false;
    }
    g_api.pushlstring(L, arg.data(), arg.size());
    if (g_api.pcall(L, 1, 1, 0) != 0) {
        err = tostring(L, -1);
        g_api.settop(L, top);
        return false;
    }
    out = tostring(L, -1);
    g_api.settop(L, top);
    return true;
}

}  // namespace lua

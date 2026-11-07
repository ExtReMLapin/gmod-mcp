// Dynamic binding to the LuaJIT C API exported by lua_shared (no import library).
#pragma once

#include <cstddef>
#include <string>

struct lua_State;
struct lua_Debug;

namespace lua {

using CFunction = int (*)(lua_State *);
using Hook = void (*)(lua_State *, lua_Debug *);

constexpr int REGISTRYINDEX = -10000;
constexpr int GLOBALSINDEX = -10002;
constexpr int MULTRET = -1;

enum Type { TNONE = -1, TNIL = 0, TBOOLEAN, TLIGHTUSERDATA, TNUMBER, TSTRING, TTABLE, TFUNCTION, TUSERDATA, TTHREAD };

struct Api {
    int (*gettop)(lua_State *);
    void (*settop)(lua_State *, int);
    void (*pushvalue)(lua_State *, int);
    void (*pushnil)(lua_State *);
    void (*pushnumber)(lua_State *, double);
    void (*pushboolean)(lua_State *, int);
    void (*pushlstring)(lua_State *, const char *, size_t);
    void (*pushstring)(lua_State *, const char *);
    void (*pushcclosure)(lua_State *, CFunction, int);
    void (*pushlightuserdata)(lua_State *, void *);
    void (*getfield)(lua_State *, int, const char *);
    void (*setfield)(lua_State *, int, const char *);
    void (*createtable)(lua_State *, int, int);
    void (*rawget)(lua_State *, int);
    void (*rawset)(lua_State *, int);
    int (*type)(lua_State *, int);
    const char *(*tolstring)(lua_State *, int, size_t *);
    double (*tonumber)(lua_State *, int);
    int (*toboolean)(lua_State *, int);
    void *(*touserdata)(lua_State *, int);
    int (*pcall)(lua_State *, int, int, int);
    int (*loadbuffer)(lua_State *, const char *, size_t, const char *);
    void (*remove)(lua_State *, int);
    void (*insert)(lua_State *, int);
    int (*error)(lua_State *);
    int (*gc)(lua_State *, int, int);
    int (*sethook)(lua_State *, Hook, int, int);
    void (*traceback)(lua_State *, lua_State *, const char *, int);

    bool loaded = false;
    std::string missing;  // missing symbols on failure
};

// Loads the symbols from lua_shared (already loaded in the process). Idempotent.
bool load(std::string &err);
const Api &api();

// Helpers.
inline void pop(lua_State *L, int n) { api().settop(L, -n - 1); }
std::string tostring(lua_State *L, int idx);  // safe copy, "" if not a string

// Compiles and runs `code` in L in protected mode; returns the return values converted to a string if
// `out` is not null (first value only). `chunk` is used as the chunk name. Returns false and fills err on error.
bool run(lua_State *L, const std::string &code, const char *chunk, std::string *out, std::string &err);

// Calls the global function `table.func(arg)` (string arg) in protected mode, returns the returned string.
bool call_string(lua_State *L, const char *table, const char *func, const std::string &arg, std::string &out, std::string &err);

}  // namespace lua

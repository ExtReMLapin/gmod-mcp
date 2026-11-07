// Access to the three Lua states via ILuaShared (LUASHARED003). Never cache the client/server ILuaInterface.
#pragma once

#include <string>

struct lua_State;

namespace realms {

enum Realm { CLIENT = 0, SERVER = 1, MENU = 2, COUNT = 3 };

const char *name(int realm);
int parse(const std::string &s);  // -1 if unknown

bool init(std::string &err);  // resolves CreateInterface and ILuaShared
// Returns the realm's lua_State, or nullptr if unavailable. NEVER cache it.
lua_State *state(int realm);
// Raw ILuaInterface pointer (diagnostics only).
void *iface(int realm);

}  // namespace realms

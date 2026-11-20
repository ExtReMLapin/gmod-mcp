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
// Main thread, at load: the checks that need a Lua interface (ILuaBase::Top's offset). GetLuaInterface itself is verified in init()
// (layout.h): its realms are unavailable when its code is not recognized.
void verify_layout();
// Tests only: use this object as the ILuaShared (nullptr: forget it) and start the offset checks over.
void set_shared_for_tests(void *shared);

}  // namespace realms

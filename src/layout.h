// Layout checks: before the module calls a fixed vtable slot of an engine interface, or trusts a fixed offset in an engine
// object, it checks that the code behind the slot is what the slot is believed to be (docs/REVERSE.md, "Slot checks").
// A game update that moves a slot then turns off the one feature that needs it, with a reason, instead of calling
// another function. The first such check was IFileSystem's (lua_mount::fs_slot_base, the 5 or 9 slot IAppSystem).
//
// The decisions are pure functions of the code bytes of the function behind a slot (and, for string references, of a
// reader for the text a RIP-relative address points at), so they are tested with the real bytes of the game builds
// (tests/native/slot_fixtures.h). They look at the SHAPE of a function (the calls through the object's own vtable it
// makes, the strings it refers to, the registers it moves, a tail jump), never at exact bytes or addresses, so a new
// build that recompiles the function with other registers or another address still passes, and one that changes what it
// does is refused. Windows only (the x86-64 Windows builds are the ones the slots were verified on): elsewhere every entry
// of the report is "not_applicable" and nothing is gated.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace layout {

using Bytes = std::vector<uint8_t>;
// The text at (start of the function + rel) read as a C string, "" if it cannot be read.
using StrAt = std::function<std::string(int64_t rel)>;

#ifdef _WIN32
constexpr bool kChecked = true;
#else
constexpr bool kChecked = false;
#endif

struct Check {
    bool ok = false;
    std::string why;  // when !ok: what did not match
    int slot = -1;    // some shapes name a slot of ANOTHER interface that the function calls (see each check)
};

// ---- ILuaShared (LUASHARED003) ----
// GetLuaInterface (slot 6): a plain read of the pointer array, `movzx eax,dl; mov rax,[rcx+rax*8+disp]; ret`.
Check get_lua_interface_shape(const Bytes &code);
// MountLua (slot 9): calls MountLuaAdd (slot 10 of its own vtable) for the string "lua" and again for "gamemodes" (the second
// only when the path ID is not "LuaMenu").
Check mount_lua_shape(const Bytes &code, const StrAt &str_at);
// MountLuaAdd (slot 10): a helper call, then `IFileSystem::MarkPathIDByRequestOnly(pathID, true)` as a tail call through the
// file system object's vtable. `slot` is that IFileSystem slot.
Check mount_lua_add_shape(const Bytes &code);
// UnMountLua (slot 11): `return g_pFullFileSystem->RemoveSearchPaths(pathID)` as a tail call. `slot` is that IFileSystem slot.
Check unmount_lua_shape(const Bytes &code);
// EmptyCache (slot 17): walks the file cache list (count at +0x18, entries at +8), frees every entry and sets the count to 0.
Check empty_cache_shape(const Bytes &code);

struct IluaShared {
    bool applicable = false;  // false where the layout is not checked: nothing below is then gated
    Check get_lua_interface, mount_lua, mount_lua_add, unmount_lua, empty_cache;
    bool ok() const { return !applicable || (get_lua_interface.ok && mount_lua.ok && mount_lua_add.ok && unmount_lua.ok && empty_cache.ok); }
    // The Lua mount needs slots 9, 10 and 11 (and the file system layout); the cache emptying slot 17; the realms slot 6.
    bool mount_ok() const { return !applicable || (mount_lua.ok && mount_lua_add.ok && unmount_lua.ok); }
    bool realms_ok() const { return !applicable || get_lua_interface.ok; }
    bool cache_ok() const { return !applicable || empty_cache.ok; }
};
// Pure: the verdict of every slot, from the code of slots 6, 9, 10, 11, 17.
IluaShared decide_iluashared(const Bytes &s6, const Bytes &s9, const StrAt &str9, const Bytes &s10, const Bytes &s11, const Bytes &s17);

// ILuaInterface slot 0 (ILuaBase::Top): `mov rcx,[rcx+state_offset]; jmp lua_gettop`: the lua_State* offset.
Check lua_top_shape(const Bytes &code, int state_offset);

// ---- IFileSystem (VFileSystem022) ----
struct FsLayout {
    int first_slot = -1;  // first IFileSystem slot after IAppSystem's: 5 or 9, -1 unknown
    std::string why;
    bool ok() const { return first_slot > 0; }
};
// Pure: the layout from the code of slots 7, 8, 11, 12 (lua_mount::fs_slot_base), then checked against what lua_shared itself
// calls through the same object: UnMountLua tail-calls RemoveSearchPaths (first_slot + 5) and MountLuaAdd tail-calls
// MarkPathIDByRequestOnly (first_slot + 6); `unmount_slot` / `mark_slot` are those slots as read from lua_shared (-1 = unknown,
// not compared). A disagreement is refused: one of the two readings is wrong.
FsLayout decide_filesystem(const Bytes &s7, const Bytes &s8, const Bytes &s11, const Bytes &s12, int unmount_slot, int mark_slot);

// ---- Verdicts for `status` ----
enum class State { Pending, Verified, Refused, NotApplicable };
// Publishes the verdict of one interface ("ifilesystem", "iluashared", "ivengineclient", "inetchannelinfo", "lua_offsets"):
// {ok, state, reason?, ...extra}. `ok` is false only for Refused. A Refused verdict is logged once (one line).
void publish(const std::string &name, State state, const std::string &reason = "", nlohmann::json extra = nlohmann::json::object());
// {"ifilesystem": {...}, "iluashared": {...}, "ivengineclient": {...}, "inetchannelinfo": {...}, "lua_offsets": {...}}. An
// interface nobody published yet is "pending" (it is checked before its first use); every entry is "not_applicable" where
// the layout is not checked.
nlohmann::json report();
// The same entry as a value (for a verdict that is computed from live counters when `status` asks: see set_provider).
nlohmann::json make_entry(State state, const std::string &reason = "", nlohmann::json extra = nlohmann::json::object());
// An interface whose verdict changes while the game runs (the lua_State offsets count each check) gives report() a function to ask.
// It replaces what was published under that name. Called from any thread.
void set_provider(const std::string &name, std::function<nlohmann::json()> fn);
// Tests only: forget every published verdict, provider and cached check.
void reset_for_tests();

// ---- The engine side (Windows): read the code behind a slot of a live object and decide ----
// ILuaShared: checked once per process (the object is a singleton), published as "iluashared". Elsewhere: not applicable (everything ok).
const IluaShared &iluashared(void *obj);
// The first bytes of the function at vtable slot `slot` of `obj` (up to `n`, as many as are readable; empty if the slot is not).
Bytes slot_code(void *obj, int slot, size_t n = 16);
// Same, for a function address.
Bytes code_at(const void *fn, size_t n);
// The reader of the text at a function's RIP-relative references.
StrAt str_reader(const void *fn);

}  // namespace layout

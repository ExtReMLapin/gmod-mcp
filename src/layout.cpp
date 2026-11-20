#include "layout.h"

#include <algorithm>
#include <map>
#include <mutex>

#include "log.h"
#include "lua_mount.h"
#include "platform/platform.h"

namespace layout {

using nlohmann::json;

namespace {

// ---- A tiny pattern reader for the few x86-64 shapes the checks look for ----

int at(const Bytes &c, size_t i) { return i < c.size() ? c[i] : -1; }
int32_t le32(const Bytes &c, size_t i) {
    return (int32_t)((uint32_t)at(c, i) | (uint32_t)at(c, i + 1) << 8 | (uint32_t)at(c, i + 2) << 16 | (uint32_t)at(c, i + 3) << 24);
}

// Length of a small function: up to the first int3 (the padding MSVC puts between functions) that follows an instruction which ends
// a function (ret, ret imm16, jmp rel32, jmp [reg+disp], jmp reg), or the whole buffer. An 0xcc inside an operand (a displacement)
// does not end it.
size_t fn_len(const Bytes &c) {
    for (size_t i = 1; i < c.size(); ++i) {
        if (c[i] != 0xcc) continue;
        bool ends = c[i - 1] == 0xc3 || (i >= 3 && c[i - 3] == 0xc2) || (i >= 5 && c[i - 5] == 0xe9) || (i >= 3 && c[i - 3] == 0xff && (c[i - 2] & 0xf8) == 0x60) ||
                    (i >= 6 && c[i - 6] == 0xff && (c[i - 5] & 0xf8) == 0xa0) || (i >= 2 && c[i - 2] == 0xff && (c[i - 1] & 0xf8) == 0xe0);
        if (ends) return i;
    }
    return c.size();
}

// A call through a vtable: `call [reg+disp8]` (ff /2, mod 01) or `call [reg+disp32]` (mod 10), optional REX.B before. The slot is
// disp / 8 (-1 if disp is not a multiple of 8 or negative).
struct VCall {
    size_t pos;
    int slot;
};
std::vector<VCall> vcalls(const Bytes &c, size_t n) {
    std::vector<VCall> out;
    for (size_t i = 0; i + 2 < n; ++i) {
        if (c[i] != 0xff) continue;
        int m = c[i + 1];
        if ((m & 0x38) != 0x10 || (m & 7) == 4) continue;
        int mod = m >> 6;
        int64_t d;
        if (mod == 1) d = (int8_t)at(c, i + 2);
        else if (mod == 2 && i + 5 < n) d = le32(c, i + 2);
        else continue;
        out.push_back({i, d >= 0 && d % 8 == 0 ? (int)(d / 8) : -1});
    }
    return out;
}

// `lea reg,[rip+rel32]` (REX.W, 8d, modrm 00 reg 101): the target as an offset from the start of the function.
std::vector<int64_t> rip_leas(const Bytes &c, size_t n) {
    std::vector<int64_t> out;
    for (size_t i = 0; i + 7 <= n; ++i)
        if ((c[i] == 0x48 || c[i] == 0x4c) && c[i + 1] == 0x8d && (c[i + 2] & 0xc7) == 0x05) out.push_back((int64_t)i + 7 + le32(c, i + 3));
    return out;
}

// A tail jump through the vtable of the object in rax: `jmp [rax+disp8]` (48 ff 60 xx) or `jmp [rax+disp32]` (48 ff a0 xxxxxxxx)
// ending the function at `n`. Returns the slot (disp / 8) or -1.
int tail_vjmp_slot(const Bytes &c, size_t n) {
    if (n >= 4 && at(c, n - 4) == 0x48 && at(c, n - 3) == 0xff && at(c, n - 2) == 0x60) {
        int d = (int8_t)at(c, n - 1);
        return d >= 0 && d % 8 == 0 ? d / 8 : -1;
    }
    if (n >= 7 && at(c, n - 7) == 0x48 && at(c, n - 6) == 0xff && at(c, n - 5) == 0xa0) {
        int32_t d = le32(c, n - 4);
        return d >= 0 && d % 8 == 0 ? d / 8 : -1;
    }
    return -1;
}

Check fail(const std::string &why) {
    Check r;
    r.why = why;
    return r;
}
Check pass(int slot = -1) {
    Check r;
    r.ok = true;
    r.slot = slot;
    return r;
}

}  // namespace

// ---- ILuaShared ----

Check get_lua_interface_shape(const Bytes &c) {
    // movzx eax,dl ; mov rax,[rcx+rax*8+disp8|disp32] ; ret
    if (at(c, 0) != 0x0f || at(c, 1) != 0xb6 || at(c, 2) != 0xc2) return fail("does not start with movzx eax,dl");
    if (at(c, 3) == 0x48 && at(c, 4) == 0x8b && at(c, 5) == 0x44 && at(c, 6) == 0xc1 && at(c, 8) == 0xc3) return pass();
    if (at(c, 3) == 0x48 && at(c, 4) == 0x8b && at(c, 5) == 0x84 && at(c, 6) == 0xc1 && at(c, 11) == 0xc3) return pass();
    return fail("is not a plain read of [this + realm*8 + offset]");
}

Check mount_lua_shape(const Bytes &c, const StrAt &str_at) {
    size_t n = fn_len(c);
    if (n < 0x20) return fail("too short for MountLua");
    auto calls = vcalls(c, n);
    if (calls.size() != 2) return fail("does not call MountLuaAdd twice");
    if (calls[0].slot != 10 || calls[1].slot != 10) return fail("does not call vtable slot 10 (MountLuaAdd)");
    auto refs = rip_leas(c, n);
    if (refs.size() < 2) return fail("does not refer to its strings");
    std::string first = str_at ? str_at(refs.front()) : "", last = str_at ? str_at(refs.back()) : "";
    if (first != "lua") return fail("its first string is not \"lua\"");
    if (last != "gamemodes") return fail("its last string is not \"gamemodes\"");
    return pass();
}

Check mount_lua_add_shape(const Bytes &c) {
    size_t n = fn_len(c);
    if (n < 0x10 || n > 0x60) return fail("not the size of MountLuaAdd's wrapper");
    bool helper_call = false, mark_true = false, fs_global = false;
    for (size_t i = 0; i + 5 <= n && i < 0x10; ++i)
        if (c[i] == 0xe8) helper_call = true;
    for (size_t i = 0; i + 3 <= n; ++i) {
        if (c[i] == 0x41 && c[i + 1] == 0xb0 && c[i + 2] == 0x01) mark_true = true;  // mov r8b,1
        if (c[i] == 0x48 && c[i + 1] == 0x8b && c[i + 2] == 0x0d) fs_global = true;  // mov rcx,[rip+g_pFullFileSystem]
    }
    if (!helper_call) return fail("does not call its helper first");
    if (!fs_global) return fail("does not load the file system object");
    if (!mark_true) return fail("does not pass true as the third argument");
    int slot = tail_vjmp_slot(c, n);
    if (slot < 0) return fail("does not end with a tail call through the file system's vtable");
    return pass(slot);
}

Check unmount_lua_shape(const Bytes &c) {
    size_t n = fn_len(c);
    // mov rcx,[rip+g_pFullFileSystem] ; mov rax,[rcx] ; jmp [rax+disp]
    if (n != 14 && n != 17) return fail("not the size of UnMountLua's wrapper");
    if (at(c, 0) != 0x48 || at(c, 1) != 0x8b || at(c, 2) != 0x0d) return fail("does not load the file system object");
    if (at(c, 7) != 0x48 || at(c, 8) != 0x8b || at(c, 9) != 0x01) return fail("does not read its vtable");
    int slot = tail_vjmp_slot(c, n);
    if (slot < 0) return fail("does not end with a tail call through the file system's vtable");
    return pass(slot);
}

Check empty_cache_shape(const Bytes &c) {
    if (c.size() < 0x100) return fail("code shorter than EmptyCache");
    // Loops over the file cache list: count (dword at +0x18) compared, and set to 0 at the end; entries freed with one call
    // target used several times; a backward conditional jump.
    bool cmp_count = false, store_count = false, back_jump = false;
    for (size_t i = 0; i + 3 <= c.size(); ++i) {
        // cmp [reg+0x18],r32 (39 /r) or cmp r32,[reg+0x18] (3b /r), mod 01, no SIB, disp 0x18; REX.R/B (0x40..0x4f) may precede.
        if ((c[i] == 0x39 || c[i] == 0x3b) && (c[i + 1] & 0xc0) == 0x40 && (c[i + 1] & 7) != 4 && c[i + 2] == 0x18) cmp_count = true;
        if (c[i] == 0x89 && (c[i + 1] & 0xc0) == 0x40 && (c[i + 1] & 7) != 4 && c[i + 2] == 0x18) store_count = true;  // mov [reg+0x18],r32
        if (i + 6 <= c.size() && c[i] == 0x0f && c[i + 1] >= 0x80 && c[i + 1] <= 0x8f) {
            int32_t rel = le32(c, i + 2);
            if (rel < -0x20 && rel > -0x200) back_jump = true;
        }
    }
    std::map<int64_t, int> targets;
    int most = 0;
    for (size_t i = 0; i + 5 <= c.size(); ++i)
        if (c[i] == 0xe8) most = std::max(most, ++targets[(int64_t)i + 5 + le32(c, i + 1)]);
    if (!cmp_count) return fail("does not compare the list count at +0x18");
    if (!store_count) return fail("does not reset the list count at +0x18");
    if (!back_jump) return fail("has no loop");
    if (most < 3) return fail("does not free entries the way EmptyCache does");
    return pass();
}

IluaShared decide_iluashared(const Bytes &s6, const Bytes &s9, const StrAt &str9, const Bytes &s10, const Bytes &s11, const Bytes &s17) {
    IluaShared r;
    r.applicable = true;
    r.get_lua_interface = get_lua_interface_shape(s6);
    r.mount_lua = mount_lua_shape(s9, str9);
    r.mount_lua_add = mount_lua_add_shape(s10);
    r.unmount_lua = unmount_lua_shape(s11);
    r.empty_cache = empty_cache_shape(s17);
    return r;
}

Check lua_top_shape(const Bytes &c, int state_offset) {
    // mov rcx,[rcx+state_offset] ; jmp lua_gettop
    if (state_offset < 0 || state_offset > 127) return fail("state offset out of the one-byte form");
    if (at(c, 0) == 0x48 && at(c, 1) == 0x8b && at(c, 2) == 0x49 && at(c, 3) == state_offset && at(c, 4) == 0xe9) return pass();
    return fail("ILuaBase::Top does not load the lua_State from this + " + std::to_string(state_offset));
}

// ---- IFileSystem ----

FsLayout decide_filesystem(const Bytes &s7, const Bytes &s8, const Bytes &s11, const Bytes &s12, int unmount_slot, int mark_slot) {
    FsLayout r;
    int base = lua_mount::fs_slot_base(s7, s8, s11, s12);
    if (base < 0) {
        r.why = "the code at slots 7, 8, 11 and 12 matches neither the 5-slot nor the 9-slot IAppSystem layout";
        return r;
    }
    if (unmount_slot >= 0 && unmount_slot != base + 5) {
        r.why = "lua_shared removes search paths through slot " + std::to_string(unmount_slot) + ", this layout puts RemoveSearchPaths at " +
                std::to_string(base + 5);
        return r;
    }
    if (mark_slot >= 0 && mark_slot != base + 6) {
        r.why = "lua_shared marks path IDs through slot " + std::to_string(mark_slot) + ", this layout puts MarkPathIDByRequestOnly at " +
                std::to_string(base + 6);
        return r;
    }
    r.first_slot = base;
    return r;
}

// ---- IVEngineClient / INetChannelInfo ----

Check client_cmd_shape(const Bytes &c) {
    size_t n = fn_len(c);
    if (n < 0x10 || n > 0x80) return fail("not the size of ClientCmd");
    // The text (rdx) is kept in a callee-saved register R, later passed as the first argument (mov rcx,R) of a tail jump.
    static const int kSaved[] = {3, 5, 6, 7};  // rbx, rbp, rsi, rdi
    for (int R : kSaved) {
        size_t keep = n;
        for (size_t i = 0; i + 3 <= n && i < 0x20; ++i) {
            bool a = c[i] == 0x48 && c[i + 1] == 0x8b && c[i + 2] == (0xc0 | (R << 3) | 2);  // mov R,rdx
            bool b = c[i] == 0x48 && c[i + 1] == 0x89 && c[i + 2] == (0xc0 | (2 << 3) | R);  // mov R,rdx (store form)
            if (a || b) {
                keep = i;
                break;
            }
        }
        if (keep == n) continue;
        for (size_t i = keep + 3; i + 3 <= n; ++i) {
            bool a = c[i] == 0x48 && c[i + 1] == 0x8b && c[i + 2] == (0xc0 | (1 << 3) | R);  // mov rcx,R
            bool b = c[i] == 0x48 && c[i + 1] == 0x89 && c[i + 2] == (0xc0 | (R << 3) | 1);  // mov rcx,R (store form)
            if (!(a || b)) continue;
            for (size_t j = i + 3; j + 5 <= n && j < i + 3 + 12; ++j)
                if (c[j] == 0xe9) return pass();  // the tail jump into the command buffer
        }
    }
    return fail("does not hand its text to a tail call");
}

Check get_net_channel_info_shape(const Bytes &c) {
    // mov rax,[rip+disp32] ; ret
    if (at(c, 0) == 0x48 && at(c, 1) == 0x8b && at(c, 2) == 0x05 && at(c, 7) == 0xc3) return pass();
    return fail("does not return a global pointer");
}

Check get_address_shape(const Bytes &c) {
    // add rcx,off (48 81 c1 imm32 | 48 83 c1 imm8) or lea rcx,[rcx+off] (48 8d 89 imm32 | 48 8d 49 imm8)
    size_t i = 0;
    if (at(c, 0) == 0x48 && at(c, 1) == 0x81 && at(c, 2) == 0xc1) i = 7;
    else if (at(c, 0) == 0x48 && at(c, 1) == 0x83 && at(c, 2) == 0xc1) i = 4;
    else if (at(c, 0) == 0x48 && at(c, 1) == 0x8d && at(c, 2) == 0x89) i = 7;
    else if (at(c, 0) == 0x48 && at(c, 1) == 0x8d && at(c, 2) == 0x49) i = 4;
    else return fail("does not take the address of a member of this");
    if (!((at(c, i) == 0x33 || at(c, i) == 0x31) && at(c, i + 1) == 0xd2)) return fail("does not pass false as the second argument");  // xor edx,edx
    if (at(c, i + 2) != 0xe9) return fail("does not tail-jump to the formatter");
    return pass();
}

bool address_text_plausible(const std::string &s) {
    if (s.empty() || s.size() > 63) return false;
    for (unsigned char ch : s)
        if (ch < 0x20 || ch > 0x7e) return false;
    return true;
}

// ---- Verdicts ----

namespace {
std::mutex g_mu;
std::map<std::string, json> g_entries;
std::map<std::string, std::string> g_logged;  // last reason logged per interface
std::map<std::string, std::function<json()>> g_providers;

const char *state_name(State s) {
    switch (s) {
        case State::Pending: return "pending";
        case State::Verified: return "verified";
        case State::Refused: return "refused";
        case State::NotApplicable: return "not_applicable";
    }
    return "?";
}

std::mutex g_ilua_mu;
std::map<void *, IluaShared> g_ilua_cache;  // one per ILuaShared object (a singleton: one per process)

const char *const kNames[] = {"ifilesystem", "iluashared", "ivengineclient", "inetchannelinfo", "lua_offsets"};
}  // namespace

json make_entry(State state, const std::string &reason, json extra) {
    json e = extra.is_object() ? std::move(extra) : json::object();
    e["state"] = state_name(state);
    e["ok"] = state != State::Refused;
    if (!reason.empty()) e["reason"] = reason;
    return e;
}

void set_provider(const std::string &name, std::function<json()> fn) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (fn) g_providers[name] = std::move(fn);
    else g_providers.erase(name);
}

void publish(const std::string &name, State state, const std::string &reason, json extra) {
    json e = make_entry(state, reason, std::move(extra));
    bool log = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_entries[name] = std::move(e);
        if (state == State::Refused) {
            std::string &last = g_logged[name];
            if (last != reason) {
                last = reason;
                log = true;
            }
        } else {
            g_logged.erase(name);
        }
    }
    if (log) logx::warn("layout check: %s: %s", name.c_str(), reason.c_str());
}

json report() {
    json r = json::object();
    std::map<std::string, std::function<json()>> providers;
    std::map<std::string, json> entries;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        providers = g_providers;
        entries = g_entries;
    }
    for (const char *n : kNames) {
        auto pv = providers.find(n);
        auto it = entries.find(n);
        if (!kChecked) r[n] = make_entry(State::NotApplicable);
        else if (pv != providers.end()) r[n] = pv->second();
        else if (it != entries.end()) r[n] = it->second;
        else r[n] = make_entry(State::Pending);
    }
    return r;
}

void reset_for_tests() {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_entries.clear();
        g_logged.clear();
        g_providers.clear();
    }
    std::lock_guard<std::mutex> lk(g_ilua_mu);
    g_ilua_cache.clear();
}

// ---- Reading a live object ----

Bytes code_at(const void *fn, size_t n) {
    if (!fn) return {};
    Bytes b(n);
    while (n >= 8) {
        b.resize(n);
        if (plat::safe_read(fn, b.data(), n)) return b;
        n /= 2;
    }
    return {};
}

Bytes slot_code(void *obj, int slot, size_t n) {
    void **vt = nullptr, *fn = nullptr;
    if (!obj || slot < 0 || !plat::safe_read(obj, &vt, sizeof vt) || !vt || !plat::safe_read(&vt[slot], &fn, sizeof fn) || !fn) return {};
    return code_at(fn, n);
}

StrAt str_reader(const void *fn) {
    return [fn](int64_t rel) -> std::string {
        std::string s;
        const char *p = static_cast<const char *>(fn) + rel;
        for (int i = 0; i < 32; ++i) {
            char ch = 0;
            if (!plat::safe_read(p + i, &ch, 1)) return "";
            if (!ch) return s;
            s.push_back(ch);
        }
        return s;  // not terminated within 32 bytes: none of the strings looked for is that long, it will not match
    };
}

const IluaShared &iluashared(void *obj) {
    std::lock_guard<std::mutex> lk(g_ilua_mu);
    auto &cache = g_ilua_cache;
    auto it = cache.find(obj);
    if (it != cache.end()) return it->second;
    IluaShared r;
    if (!kChecked || !obj) {
        r.applicable = false;
        return cache.emplace(obj, r).first->second;
    }
    void **vt = nullptr, *fn9 = nullptr;
    plat::safe_read(obj, &vt, sizeof vt);
    if (vt) plat::safe_read(&vt[9], &fn9, sizeof fn9);
    r = decide_iluashared(slot_code(obj, 6, 16), slot_code(obj, 9, 0x60), str_reader(fn9), slot_code(obj, 10, 0x40), slot_code(obj, 11, 16),
                          slot_code(obj, 17, 0x200));
    json extra{{"get_lua_interface", r.get_lua_interface.ok}, {"mount_lua", r.mount_lua.ok}, {"mount_lua_add", r.mount_lua_add.ok},
               {"unmount_lua", r.unmount_lua.ok}, {"empty_cache", r.empty_cache.ok}};
    if (r.mount_lua_add.ok) extra["file_system_mark_slot"] = r.mount_lua_add.slot;
    if (r.unmount_lua.ok) extra["file_system_remove_slot"] = r.unmount_lua.slot;
    std::string why;
    auto add = [&](const char *name, int slot, const Check &c) {
        if (c.ok) return;
        why += (why.empty() ? "" : "; ") + std::string("slot ") + std::to_string(slot) + " (" + name + ") " + c.why;
    };
    add("GetLuaInterface", 6, r.get_lua_interface);
    add("MountLua", 9, r.mount_lua);
    add("MountLuaAdd", 10, r.mount_lua_add);
    add("UnMountLua", 11, r.unmount_lua);
    add("EmptyCache", 17, r.empty_cache);
    publish("iluashared", why.empty() ? State::Verified : State::Refused, why, extra);
    return cache.emplace(obj, r).first->second;
}

}  // namespace layout

// Slot checks (docs/TESTS.md SL-*): the module verifies every fixed vtable slot and offset it uses before the first use.
// The decisions are tested with the REAL bytes of the game builds (tests/native/slot_fixtures.h, generated from the files on
// disk by tests/native/tools/gen_slot_fixtures.py); the glue (reading a live object, refusing, never calling an unverified
// slot) is tested on Windows against fake objects whose vtable slots hold those real bytes (or small stubs that run).
#include <algorithm>
#include <cstdio>
#include <map>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "check.h"
#include "layout.h"
#include "lua_mount.h"
#include "realms.h"
#include "slot_fixtures.h"

using layout::Bytes;
using nlohmann::json;

namespace {

Bytes code(const slotfx::Slots &s, int slot) {
    auto it = s.code.find(slot);
    return it == s.code.end() ? Bytes{} : slotfx::bytes(it->second);
}
layout::StrAt strings(const std::map<int64_t, const char *> &m) {
    return [&m](int64_t rel) -> std::string {
        auto it = m.find(rel);
        return it == m.end() ? "" : it->second;
    };
}

// The slots of `s` (0..max) that `shape` accepts.
template <typename F>
std::set<int> accepted(const slotfx::Slots &s, int max, F shape) {
    std::set<int> out;
    for (int i = 0; i <= max; ++i)
        if (shape(code(s, i)).ok) out.insert(i);
    return out;
}

const slotfx::Slots *const kLuaShared[] = {&slotfx::luashared_old, &slotfx::luashared_new};
const std::map<int64_t, const char *> *const kLuaSharedStrings[] = {&slotfx::luashared_old_strings, &slotfx::luashared_new_strings};
const char *const kTop[] = {slotfx::luainterface_old_top, slotfx::luainterface_new_top};

// SL-1: the ILuaShared slots, real bytes of both lua_shared builds: each shape accepts its own slot and no other.
void test_iluashared_shapes() {
    using namespace layout;
    for (int b = 0; b < 2; ++b) {
        const auto &s = *kLuaShared[b];
        const auto &strs = *kLuaSharedStrings[b];
        CHECK(get_lua_interface_shape(code(s, 6)).ok);
        CHECK(accepted(s, 18, get_lua_interface_shape) == std::set<int>{6});
        CHECK(mount_lua_shape(code(s, 9), strings(strs)).ok);
        CHECK(accepted(s, 18, [&](const Bytes &c) { return mount_lua_shape(c, strings(strs)); }) == std::set<int>{9});
        Check add = mount_lua_add_shape(code(s, 10));
        CHECK(add.ok);
        CHECK(accepted(s, 18, mount_lua_add_shape) == std::set<int>{10});
        Check un = unmount_lua_shape(code(s, 11));
        CHECK(un.ok);
        CHECK(accepted(s, 18, unmount_lua_shape) == std::set<int>{11});
        CHECK(empty_cache_shape(code(s, 17)).ok);
        CHECK(accepted(s, 18, empty_cache_shape) == std::set<int>{17});
        // The IFileSystem slots lua_shared itself calls: RemoveSearchPaths = first slot + 5, MarkPathIDByRequestOnly = + 6.
        CHECK(b == 0 ? (un.slot == 10 && add.slot == 11) : (un.slot == 14 && add.slot == 15));
        CHECK(lua_top_shape(slotfx::bytes(kTop[b]), 8).ok);
        CHECK(!lua_top_shape(slotfx::bytes(kTop[b]), 16).ok);  // another offset is not what the code reads
    }
    // A build that recompiles with other registers still passes (shape, not bytes): GetLuaInterface with disp32.
    CHECK(layout::get_lua_interface_shape(Bytes{0x0f, 0xb6, 0xc2, 0x48, 0x8b, 0x84, 0xc1, 0x80, 0x00, 0x00, 0x00, 0xc3}).ok);
    // ... and one that changes what the function does is refused.
    CHECK(!layout::get_lua_interface_shape(Bytes{0x0f, 0xb6, 0xc2, 0x48, 0x8b, 0x44, 0xc1, 0x78}).ok);       // no ret
    CHECK(!layout::get_lua_interface_shape(Bytes{0x0f, 0xb6, 0xc2, 0x48, 0x8d, 0x44, 0xc1, 0x78, 0xc3}).ok);  // lea: address, not the read
    CHECK(!layout::get_lua_interface_shape(Bytes{}).ok);
    // MountLua mutations: strings swapped, another slot, one call, no strings, unreadable text.
    Bytes m = code(slotfx::luashared_new, 9);
    const auto &ms = slotfx::luashared_new_strings;
    layout::StrAt swapped = [&](int64_t r) { auto s = strings(ms)(r); return s == "lua" ? std::string("gamemodes") : s == "gamemodes" ? std::string("lua") : s; };
    CHECK(!layout::mount_lua_shape(m, swapped).ok);
    CHECK(!layout::mount_lua_shape(m, nullptr).ok);
    CHECK(!layout::mount_lua_shape(m, [](int64_t) { return std::string(); }).ok);
    Bytes other_slot = m;
    for (size_t i = 0; i + 2 < other_slot.size(); ++i)
        if (other_slot[i] == 0xff && other_slot[i + 1] == 0x50 && other_slot[i + 2] == 0x50) other_slot[i + 2] = 0x58;  // slot 11
    CHECK(!layout::mount_lua_shape(other_slot, strings(ms)).ok);
    Bytes one_call = m;
    for (size_t i = 0; i + 2 < one_call.size(); ++i)
        if (one_call[i] == 0xff && one_call[i + 1] == 0x50 && one_call[i + 2] == 0x50) { one_call[i] = 0x90; one_call[i + 1] = 0x90; one_call[i + 2] = 0x90; break; }
    CHECK(!layout::mount_lua_shape(one_call, strings(ms)).ok);
    CHECK(!layout::mount_lua_shape(Bytes(m.begin(), m.begin() + 16), strings(ms)).ok);  // truncated
    // EmptyCache truncated (unreadable tail) is not accepted either.
    Bytes ec = code(slotfx::luashared_old, 17);
    CHECK(!layout::empty_cache_shape(Bytes(ec.begin(), ec.begin() + 0x40)).ok);
}

// SL-2: one verdict per slot: a bad slot only turns off what needs it.
void test_iluashared_decision() {
    using namespace layout;
    for (int b = 0; b < 2; ++b) {
        const auto &s = *kLuaShared[b];
        auto str = strings(*kLuaSharedStrings[b]);
        IluaShared r = decide_iluashared(code(s, 6), code(s, 9), str, code(s, 10), code(s, 11), code(s, 17));
        CHECK(r.applicable && r.ok() && r.mount_ok());
        // Slot 9 unknown: the mount is refused, the realms and the cache emptying are not affected.
        IluaShared bad9 = decide_iluashared(code(s, 6), code(s, 10), str, code(s, 10), code(s, 11), code(s, 17));
        CHECK(!bad9.mount_lua.ok && !bad9.ok() && !bad9.mount_ok() && bad9.get_lua_interface.ok && bad9.empty_cache.ok);
        // Slot 17 unknown: the cache emptying is off, the mount slots are fine.
        IluaShared bad17 = decide_iluashared(code(s, 6), code(s, 9), str, code(s, 10), code(s, 11), code(s, 16));
        CHECK(!bad17.empty_cache.ok && !bad17.ok() && bad17.mount_ok() && bad17.get_lua_interface.ok);
        // Slot 6 unknown: only the realms are off.
        IluaShared bad6 = decide_iluashared(code(s, 8), code(s, 9), str, code(s, 10), code(s, 11), code(s, 17));
        CHECK(!bad6.get_lua_interface.ok && bad6.mount_ok() && bad6.empty_cache.ok);
        CHECK(!bad6.get_lua_interface.why.empty());  // says what did not match
    }
    CHECK(IluaShared{}.ok());  // not applicable (nothing checked): nothing is gated
}

// SL-3: IFileSystem: the layout from slots 7/8/11/12 of the two real filesystem_stdio builds, confirmed by what lua_shared itself calls.
void test_filesystem_decision() {
    using namespace layout;
    const auto &o = slotfx::filesystem_old;
    const auto &n = slotfx::filesystem_new;
    FsLayout r = decide_filesystem(code(o, 7), code(o, 8), code(o, 11), code(o, 12), -1, -1);
    CHECK(r.ok() && r.first_slot == 5);
    r = decide_filesystem(code(n, 7), code(n, 8), code(n, 11), code(n, 12), -1, -1);
    CHECK(r.ok() && r.first_slot == 9);
    // lua_shared of the same build agrees (UnMountLua -> slot first+5, MountLuaAdd -> first+6).
    CHECK(decide_filesystem(code(o, 7), code(o, 8), code(o, 11), code(o, 12), 10, 11).first_slot == 5);
    CHECK(decide_filesystem(code(n, 7), code(n, 8), code(n, 11), code(n, 12), 14, 15).first_slot == 9);
    // A lua_shared of the other build disagrees: refused, with the two slots named.
    r = decide_filesystem(code(n, 7), code(n, 8), code(n, 11), code(n, 12), 10, 11);
    CHECK(!r.ok() && r.first_slot == -1 && r.why.find("10") != std::string::npos && r.why.find("14") != std::string::npos);
    r = decide_filesystem(code(o, 7), code(o, 8), code(o, 11), code(o, 12), 10, 15);
    CHECK(!r.ok() && r.why.find("15") != std::string::npos);
    // Unrecognized code: refused with a reason.
    r = decide_filesystem(code(n, 9), code(n, 10), code(n, 9), code(n, 10), -1, -1);
    CHECK(!r.ok() && !r.why.empty());
    CHECK(!decide_filesystem(Bytes{}, code(n, 8), code(n, 11), code(n, 12), -1, -1).ok());
    // The real code: slot 7/8 of the new build are trivial, the old build's are not.
    CHECK(lua_mount::is_constant_return(code(n, 7)) && lua_mount::is_constant_return(code(n, 8)));
    CHECK(!lua_mount::is_constant_return(code(o, 7)) && !lua_mount::is_constant_return(code(o, 8)));
}

// SL-5: the verdicts for status.
void test_report() {
    using namespace layout;
    reset_for_tests();
    json r = report();
    const char *names[] = {"ifilesystem", "iluashared", "ivengineclient", "inetchannelinfo", "lua_offsets"};
    for (const char *n : names) {
        CHECK(r.contains(n) && r[n].value("ok", false));  // nothing published yet: pending, nothing refused
        CHECK(r[n].value("state", "") == (kChecked ? "pending" : "not_applicable"));
    }
    if (!kChecked) return;
    publish("ifilesystem", State::Verified, "", json{{"first_slot", 9}});
    publish("ivengineclient", State::Refused, "slot 7 (ClientCmd) does not hand its text to a tail call", json{{"client_cmd", false}});
    publish("lua_offsets", State::NotApplicable);
    r = report();
    CHECK(r["ifilesystem"] == (json{{"first_slot", 9}, {"state", "verified"}, {"ok", true}}));
    CHECK(r["ivengineclient"]["ok"] == false && r["ivengineclient"]["state"] == "refused" && r["ivengineclient"]["client_cmd"] == false);
    CHECK(r["ivengineclient"]["reason"].get<std::string>().find("ClientCmd") != std::string::npos);
    CHECK(r["lua_offsets"]["state"] == "not_applicable" && r["lua_offsets"]["ok"] == true);
    CHECK(r["iluashared"]["state"] == "pending" && r["inetchannelinfo"]["state"] == "pending");
    reset_for_tests();
}

#ifdef _WIN32

// A page that executes: stubs with the real shapes that run, for the glue tests.
struct ExecPage {
    uint8_t *p;
    ExecPage() : p(static_cast<uint8_t *>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))) { std::memset(p, 0xcc, 0x1000); }
    ~ExecPage() { if (p) VirtualFree(p, 0, MEM_RELEASE); }
    void put(size_t off, const Bytes &b) { std::memcpy(p + off, b.data(), b.size()); }
    void put64(size_t off, uint64_t v) { std::memcpy(p + off, &v, 8); }
};
// An object whose vtable holds `slots` (pointers), 0x100 bytes of its own after the vptr.
struct FakeObj {
    std::vector<void *> vt;
    std::vector<uint8_t> body = std::vector<uint8_t>(0x200);
    explicit FakeObj(size_t n) : vt(n, nullptr) { void *v = vt.data(); std::memcpy(body.data(), &v, 8); }
    void *obj() { return body.data(); }
};

// SL-6: ILuaShared read from a live (fake) object with the real bytes in its vtable.
void test_iluashared_glue() {
    using namespace layout;
    reset_for_tests();
    for (int b = 0; b < 2; ++b) {
        const auto &s = *kLuaShared[b];
        const auto &strs = *kLuaSharedStrings[b];
        FakeObj fo(19);
        std::vector<std::vector<uint8_t>> bufs(19);
        for (int i = 0; i < 19; ++i) {
            bufs[i] = code(s, i);
            if (i == 9) {  // the strings sit far from the function: at their real offsets
                bufs[i].resize(0x90000);
                Bytes c9 = code(s, 9);
                std::copy(c9.begin(), c9.end(), bufs[i].begin());
                for (const auto &kv : strs) std::memcpy(bufs[i].data() + kv.first, kv.second, std::strlen(kv.second) + 1);
            }
            bufs[i].resize(std::max<size_t>(bufs[i].size(), 0x300));
            fo.vt[i] = bufs[i].data();
        }
        const IluaShared &r = iluashared(fo.obj());
        CHECK(r.applicable && r.ok() && r.mount_ok());
        json rep = report()["iluashared"];
        CHECK(rep["ok"] == true && rep["state"] == "verified" && rep["mount_lua"] == true && rep["get_lua_interface"] == true);
        CHECK(rep["file_system_remove_slot"] == (b == 0 ? 10 : 14) && rep["file_system_mark_slot"] == (b == 0 ? 11 : 15));
        CHECK(&iluashared(fo.obj()) == &r);  // cached per object
        reset_for_tests();
    }
    // Slot 9 holds another function: the mount slots are refused with a reason, GetLuaInterface and EmptyCache are not.
    {
        const auto &s = slotfx::luashared_new;
        FakeObj fo(19);
        std::vector<std::vector<uint8_t>> bufs(19);
        for (int i = 0; i < 19; ++i) {
            bufs[i] = code(s, i == 9 ? 8 : i);
            bufs[i].resize(std::max<size_t>(bufs[i].size(), 0x300));
            fo.vt[i] = bufs[i].data();
        }
        const IluaShared &r = iluashared(fo.obj());
        CHECK(!r.mount_ok() && r.get_lua_interface.ok && r.empty_cache.ok);
        json rep = report()["iluashared"];
        CHECK(rep["ok"] == false && rep["state"] == "refused" && rep["mount_lua"] == false && rep["empty_cache"] == true);
        CHECK(rep["reason"].get<std::string>().find("MountLua") != std::string::npos);
    }
    reset_for_tests();
}

// SL-7: realms: GetLuaInterface is called only when its code is the plain read; lua_offsets counts the back-reference check.
void test_realms_glue() {
    using namespace layout;
    reset_for_tests();
    ExecPage page;
    page.put(0, code(slotfx::luashared_old, 6));  // the real GetLuaInterface: it runs
    FakeObj shared(19);
    for (auto &p : shared.vt) p = page.p + 0x800;  // anything, never called
    shared.vt[6] = page.p;
    std::vector<uint8_t> iface(0x40), L(0x100);
    void *ip = iface.data(), *Lp = L.data();
    std::memcpy(iface.data() + 8, &Lp, 8);                       // CLuaInterface: lua_State* at +8
    std::memcpy(L.data() + 0x78, &ip, 8);                        // lua_State: luabase at +0x78
    std::memcpy(shared.body.data() + 0x78 + 2 * 8, &ip, 8);      // the menu realm
    realms::set_shared_for_tests(shared.obj());
    CHECK(realms::iface(realms::MENU) == ip);
    CHECK(realms::state(realms::MENU) == reinterpret_cast<lua_State *>(Lp));
    CHECK(realms::state(realms::SERVER) == nullptr);  // no such realm
    json lo = report()["lua_offsets"];
    CHECK(lo["ok"] == true && lo["state"] == "verified" && lo["backref_ok"].get<int>() >= 1 && lo["state_offset"] == 8 && lo["luabase_offset"] == 0x78);
    CHECK(report()["iluashared"]["get_lua_interface"] == true);
    // The back reference does not match: refused (the offsets are not what they are believed to be) and counted.
    void *wrong = &page;
    std::memcpy(L.data() + 0x78, &wrong, 8);
    realms::set_shared_for_tests(shared.obj());
    CHECK(realms::state(realms::MENU) == nullptr);
    lo = report()["lua_offsets"];
    CHECK(lo["ok"] == false && lo["state"] == "refused" && lo["backref_mismatch"].get<int>() >= 1);
    // Slot 6 is another function: never called (its vtable entry is not even executable), the realms are off.
    std::vector<uint8_t> junk = code(slotfx::luashared_old, 8);
    junk.resize(0x100);
    shared.vt[6] = junk.data();
    reset_for_tests();
    realms::set_shared_for_tests(shared.obj());
    CHECK(realms::iface(realms::MENU) == nullptr);
    CHECK(realms::state(realms::MENU) == nullptr);
    json rep = report()["iluashared"];
    CHECK(rep["ok"] == false && rep["get_lua_interface"] == false && rep["reason"].get<std::string>().find("GetLuaInterface") != std::string::npos);
    realms::set_shared_for_tests(nullptr);
    reset_for_tests();
}

#endif  // _WIN32

}  // namespace

void run_slotcheck_tests(int *pass, int *fail) {
    test_iluashared_shapes();
    test_iluashared_decision();
    test_filesystem_decision();
    test_report();
#ifdef _WIN32
    test_iluashared_glue();
    test_realms_glue();
#endif
    *pass = g_pass;
    *fail = g_fail;
}

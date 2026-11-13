// LuaJIT dump reader: dumps built by hand following lj_bcwrite.c (real dumps from the game are checked live).
#include <string>
#include <vector>

#include "check.h"
#include "ljdump.h"

static std::string uleb(uint32_t v) {
    std::string s;
    do {
        unsigned char b = v & 0x7f;
        v >>= 7;
        if (v) b |= 0x80;
        s += (char)b;
    } while (v);
    return s;
}

// One prototype: `code` stands for bytecode + upvalues + constants (a multiple of 4 bytes is enough here).
static std::string proto(const std::string &code, uint32_t firstline, uint32_t numline, bool strip) {
    std::string d("\x00\x01\x02\x00", 4);  // flags, numparams, framesize, sizeuv
    d += uleb(0) + uleb(0) + uleb((uint32_t)code.size() / 4);
    std::string dbg = "lineinfo@" + std::to_string(firstline) + ":" + std::to_string(numline);
    if (!strip) d += uleb((uint32_t)dbg.size()) + uleb(firstline) + uleb(numline);
    d += code;
    if (!strip) d += dbg;
    return uleb((uint32_t)d.size()) + d;
}

static std::string dump(const std::vector<std::string> &protos, bool strip = false) {
    std::string s = "\x1bLJ\x02" + uleb(strip ? 2 : 0);
    const std::string name = "@addons/x/lua/m.lua";
    if (!strip) s += uleb((uint32_t)name.size()) + name;
    for (const auto &p : protos) s += p;
    return s + std::string(1, '\0');
}

static std::vector<ljdump::Proto> parsed(const std::string &d) {
    std::vector<ljdump::Proto> v;
    std::string err;
    ljdump::parse(d, v, err);
    return v;
}

void test_ljdump() {
    const std::string A = "AAAA", C1 = "CHLDCHLD", P1 = "PARENT01", P2 = "PARENT02", M = "MAIN";
    // The file: a() at line 3, then b() at line 4 holding a nested function at line 9, then the main chunk.
    std::string v1 = dump({proto(A, 3, 0, false), proto(C1, 9, 0, false), proto(P1, 4, 8, false), proto(M, 0, 14, false)});
    std::string live_b = dump({proto(C1, 9, 0, false), proto(P1, 4, 8, false)});

    std::vector<ljdump::Proto> p;
    std::string err;
    CHECK(ljdump::parse(v1, p, err) && p.size() == 4);
    CHECK(p.size() == 4 && p[2].firstline == 4 && p[2].numline == 8 && p[3].firstline == 0);
    // Debug info is not part of the body: the same code at another line has the same body.
    CHECK(parsed(dump({proto(P1, 4, 8, false)}))[0].body == parsed(dump({proto(P1, 40, 8, false)}))[0].body);
    CHECK(parsed(dump({proto(P1, 4, 8, false)}))[0].body != parsed(dump({proto(P2, 4, 8, false)}))[0].body);

    // Same file: found where it was loaded.
    auto m = ljdump::find(parsed(live_b), parsed(v1));
    CHECK(m.found && m.disk_line == 4);
    // A line added above: same code, now at line 5.
    std::string v2 = dump({proto(A, 4, 0, false), proto(C1, 10, 0, false), proto(P1, 5, 8, false), proto(M, 0, 15, false)});
    m = ljdump::find(parsed(live_b), parsed(v2));
    CHECK(m.found && m.disk_line == 5);
    // b() edited: not found.
    std::string v3 = dump({proto(A, 3, 0, false), proto(C1, 9, 0, false), proto(P2, 4, 8, false), proto(M, 0, 14, false)});
    CHECK(!ljdump::find(parsed(live_b), parsed(v3)).found);
    // Only its nested function edited: not found either.
    std::string v4 = dump({proto(A, 3, 0, false), proto("CHLDXXXX", 9, 0, false), proto(P1, 4, 8, false), proto(M, 0, 14, false)});
    CHECK(!ljdump::find(parsed(live_b), parsed(v4)).found);
    // The same code twice in the file: the copy at the loaded line wins.
    std::string v5 = dump({proto(C1, 2, 0, false), proto(P1, 1, 8, false), proto(C1, 9, 0, false), proto(P1, 4, 8, false), proto(M, 0, 14, false)});
    m = ljdump::find(parsed(live_b), parsed(v5));
    CHECK(m.found && m.disk_line == 4);
    // Stripped dumps parse too (no lines).
    std::vector<ljdump::Proto> s;
    CHECK(ljdump::parse(dump({proto(P1, 4, 8, true)}, true), s, err) && s.size() == 1 && s[0].firstline == 0);
    CHECK(s.size() == 1 && s[0].body == parsed(dump({proto(P1, 4, 8, false)}))[0].body);

    // Malformed input is refused, never read past the end.
    CHECK(!ljdump::parse("", p, err));
    CHECK(!ljdump::parse("hello world", p, err));
    CHECK(!ljdump::parse(std::string("\x1bLJ\x07\x00", 5), p, err));                  // unknown version
    CHECK(!ljdump::parse(v1.substr(0, v1.size() - 5), p, err));                       // truncated
    CHECK(!ljdump::parse(dump({}), p, err));                                          // no prototype
    CHECK(!ljdump::parse(std::string("\x1bLJ\x02\x00\xff\xff\xff\xff\x0f", 10), p, err));  // name longer than the dump
    CHECK(!ljdump::parse(std::string("\x1bLJ\x02\x02\x05\x00\x00", 8), p, err));      // prototype longer than the dump
    for (size_t cut = 0; cut < v1.size(); ++cut) ljdump::parse(v1.substr(0, cut), p, err);  // every prefix: no crash
    CHECK(!ljdump::find({}, parsed(v1)).found);
    CHECK(!ljdump::find(parsed(v1), parsed(live_b)).found);
}

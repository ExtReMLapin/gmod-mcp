#include "ljdump.h"

namespace ljdump {

// Format (LuaJIT 2.x, lj_bcwrite.c): "\x1bLJ", version, uleb flags, [uleb len + chunk name unless stripped], then
// prototypes, each "uleb length + data", ended by a zero length. Prototype data: flags, numparams, framesize, sizeuv
// (1 byte each), uleb sizekgc, uleb sizekn, uleb sizebc, [uleb sizedbg, and if non zero uleb firstline, uleb numline,
// unless stripped], then bytecode, upvalues, constants, and the sizedbg bytes of debug info last.
static constexpr uint32_t kFlagStrip = 0x02;

namespace {
struct Reader {
    const std::string &s;
    size_t p, end;
    bool ok = true;
    uint32_t byte() {
        if (p >= end) {
            ok = false;
            return 0;
        }
        return (unsigned char)s[p++];
    }
    uint32_t uleb() {
        uint32_t v = 0;
        for (int shift = 0; shift < 35; shift += 7) {
            uint32_t b = byte();
            if (!ok) return 0;
            v |= (b & 0x7f) << shift;
            if (!(b & 0x80)) return v;
        }
        ok = false;
        return 0;
    }
};
}  // namespace

bool parse(const std::string &dump, std::vector<Proto> &out, std::string &err) {
    out.clear();
    Reader r{dump, 0, dump.size()};
    if (dump.size() < 5 || dump.compare(0, 3, "\x1bLJ") != 0) {
        err = "not a LuaJIT bytecode dump";
        return false;
    }
    r.p = 3;
    uint32_t version = r.byte();
    if (version != 1 && version != 2) {
        err = "unsupported LuaJIT dump version " + std::to_string(version);
        return false;
    }
    uint32_t flags = r.uleb();
    bool strip = (flags & kFlagStrip) != 0;
    if (!strip) {
        uint32_t n = r.uleb();
        if (!r.ok || n > r.end - r.p) {
            err = "truncated dump header";
            return false;
        }
        r.p += n;
    }
    for (;;) {
        uint32_t len = r.uleb();
        if (!r.ok) {
            err = "truncated dump";
            return false;
        }
        if (len == 0) break;
        if (len > r.end - r.p) {
            err = "truncated prototype";
            return false;
        }
        size_t pend = r.p + len;
        Reader q{dump, r.p, pend};
        size_t h0 = q.p;
        for (int i = 0; i < 4; ++i) q.byte();
        q.uleb();  // sizekgc
        q.uleb();  // sizekn
        q.uleb();  // sizebc
        size_t h1 = q.p;
        Proto pr;
        uint32_t sizedbg = 0;
        if (!strip) {
            sizedbg = q.uleb();
            if (sizedbg) {
                pr.firstline = q.uleb();
                pr.numline = q.uleb();
            }
        }
        if (!q.ok || sizedbg > pend - q.p) {
            err = "malformed prototype header";
            return false;
        }
        pr.body.assign(dump, h0, h1 - h0);
        pr.body.append(dump, q.p, pend - sizedbg - q.p);
        out.push_back(std::move(pr));
        r.p = pend;
    }
    if (out.empty()) {
        err = "no prototype in the dump";
        return false;
    }
    return true;
}

Match find(const std::vector<Proto> &live, const std::vector<Proto> &disk) {
    Match m;
    size_t k = live.size();
    if (k == 0 || disk.size() < k) return m;
    // The live function and its nested functions are k consecutive prototypes of the file (children first).
    for (size_t i = k - 1; i < disk.size(); ++i) {
        bool same = true;
        for (size_t j = 0; j < k && same; ++j) same = disk[i + 1 - k + j].body == live[j].body;
        if (same) {
            m.found = true;
            m.disk_line = disk[i].firstline;
            if (disk[i].firstline == live.back().firstline) return m;  // prefer the same place
        }
    }
    return m;
}

}  // namespace ljdump

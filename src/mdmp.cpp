#include "mdmp.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>

namespace mdmp {

namespace {

// Stream types (MINIDUMP_STREAM_TYPE).
constexpr uint32_t kThreadList = 3, kModuleList = 4, kMemoryList = 5, kException = 6;

struct Reader {
    const std::string &d;
    bool has(uint64_t off, uint64_t n) const { return off <= d.size() && n <= d.size() - off; }
    template <class T>
    bool get(uint64_t off, T &out) const {
        if (!has(off, sizeof(T))) return false;
        std::memcpy(&out, d.data() + off, sizeof(T));  // the format is little-endian, and so is every platform this runs on
        return true;
    }
};

struct Module {
    uint64_t base = 0;
    uint32_t size = 0;
    std::string name;  // file name only
};

struct Region {
    uint64_t start = 0;
    uint64_t rva = 0;
    uint32_t size = 0;
};

// MINIDUMP_STRING: a 32-bit length in bytes, then UTF-16LE. Returns UTF-8 (BMP only: module paths).
std::string read_string(const Reader &r, uint64_t rva) {
    uint32_t bytes = 0;
    if (!r.get(rva, bytes) || bytes > 4096 || !r.has(rva + 4, bytes)) return "";
    std::string out;
    for (uint32_t i = 0; i + 1 < bytes; i += 2) {
        uint16_t c;
        r.get(rva + 4 + i, c);
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        } else {
            out += (char)(0xE0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    size_t s = out.find_last_of("\\/");
    return s == std::string::npos ? out : out.substr(s + 1);
}

std::string hex(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", (unsigned long long)v);
    return b;
}

std::string where(const std::vector<Module> &mods, uint64_t a) {
    for (const auto &m : mods)
        if (a >= m.base && a - m.base < m.size) return (m.name.empty() ? "module" : m.name) + "+" + hex(a - m.base);
    return hex(a);
}

const Module *module_of(const std::vector<Module> &mods, uint64_t a) {
    for (const auto &m : mods)
        if (a >= m.base && a - m.base < m.size) return &m;
    return nullptr;
}

}  // namespace

Facts parse(const std::string &bytes, size_t max_candidates, size_t scan_bytes) {
    Facts f;
    Reader r{bytes};
    uint32_t sig = 0, nstreams = 0, dir_rva = 0;
    if (!r.get(0, sig) || sig != 0x504d444dU) return f;  // "MDMP"
    if (!r.get(8, nstreams) || !r.get(12, dir_rva) || nstreams > 256) return f;
    struct Stream {
        uint32_t size = 0, rva = 0;
        bool present = false;
    } st[16];
    for (uint32_t i = 0; i < nstreams; ++i) {
        uint32_t type = 0, size = 0, rva = 0;
        uint64_t o = (uint64_t)dir_rva + 12ULL * i;
        if (!r.get(o, type) || !r.get(o + 4, size) || !r.get(o + 8, rva)) return f;
        if (type < 16) st[type] = Stream{size, rva, true};
    }
    // Exception stream: thread id, MINIDUMP_EXCEPTION (code, flags, record, address, ...) at +8, the thread's CONTEXT location at +160.
    if (!st[kException].present) return f;
    uint64_t ex = st[kException].rva;
    uint32_t tid = 0, ctx_size = 0, ctx_rva = 0;
    uint64_t addr = 0;
    if (!r.get(ex, tid) || !r.get(ex + 8 + 16, addr) || !r.get(ex + 160, ctx_size) || !r.get(ex + 164, ctx_rva)) return f;
    // Modules.
    std::vector<Module> mods;
    if (st[kModuleList].present) {
        uint64_t m = st[kModuleList].rva;
        uint32_t n = 0;
        if (r.get(m, n)) {
            for (uint32_t i = 0; i < n && i < 4096; ++i) {
                uint64_t o = m + 4 + 108ULL * i;
                Module mod;
                uint32_t name_rva = 0;
                if (!r.get(o, mod.base) || !r.get(o + 8, mod.size) || !r.get(o + 20, name_rva)) break;
                mod.name = read_string(r, name_rva);
                mods.push_back(mod);
            }
        }
    }
    f.ok = true;
    f.fault = where(mods, addr);
    // The stack pointer (x64 CONTEXT: Rsp at 0x98) and the memory that holds the faulting thread's stack.
    uint64_t rsp = 0;
    if (ctx_size < 0xA0 || !r.get((uint64_t)ctx_rva + 0x98, rsp) || rsp == 0) return f;
    std::vector<Region> regions;
    if (st[kThreadList].present) {
        uint64_t t = st[kThreadList].rva;
        uint32_t n = 0;
        if (r.get(t, n)) {
            for (uint32_t i = 0; i < n && i < 4096; ++i) {
                uint64_t o = t + 4 + 48ULL * i;
                uint32_t id = 0;
                Region g;
                uint32_t rva32 = 0;
                if (!r.get(o, id) || !r.get(o + 24, g.start) || !r.get(o + 32, g.size) || !r.get(o + 36, rva32)) break;
                g.rva = rva32;
                if (id == tid) regions.insert(regions.begin(), g);  // the thread's own stack first
                else regions.push_back(g);
            }
        }
    }
    if (st[kMemoryList].present) {
        uint64_t m = st[kMemoryList].rva;
        uint32_t n = 0;
        if (r.get(m, n)) {
            for (uint32_t i = 0; i < n && i < 65536; ++i) {
                uint64_t o = m + 4 + 16ULL * i;
                Region g;
                uint32_t rva32 = 0;
                if (!r.get(o, g.start) || !r.get(o + 8, g.size) || !r.get(o + 12, rva32)) break;
                g.rva = rva32;
                regions.push_back(g);
            }
        }
    }
    for (const auto &g : regions) {
        if (rsp < g.start || rsp - g.start >= g.size || !r.has(g.rva, g.size)) continue;
        uint64_t off = (rsp - g.start) & ~7ULL;
        uint64_t end = std::min<uint64_t>(g.size, off + scan_bytes);
        std::set<uint64_t> seen;
        for (; off + 8 <= end && f.candidates.size() < max_candidates; off += 8) {
            uint64_t v = 0;
            r.get(g.rva + off, v);
            const Module *m = module_of(mods, v);
            if (!m || v - m->base < 0x1000 || v == addr || !seen.insert(v).second) continue;  // a module's header, the fault itself, a repeat
            f.candidates.push_back(where(mods, v));
        }
        break;
    }
    return f;
}

Facts parse_file(const std::string &path_u8, size_t max_candidates) {
    std::ifstream in(std::filesystem::u8path(path_u8), std::ios::binary);
    if (!in) return Facts();
    in.seekg(0, std::ios::end);
    std::streamoff n = in.tellg();
    if (n <= 0 || n > 32 * 1024 * 1024) return Facts();
    std::string bytes((size_t)n, '\0');
    in.seekg(0);
    if (!in.read(bytes.data(), n)) return Facts();
    return parse(bytes, max_candidates);
}

}  // namespace mdmp

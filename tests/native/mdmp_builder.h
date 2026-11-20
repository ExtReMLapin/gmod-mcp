// A minidump built by hand following the MINIDUMP_* layouts (x64, as the game writes them), for the tests of src/mdmp.cpp and of the
// crash answers that quote it. Only what the reader looks at is filled in: header, thread list, module list, memory list, exception.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace mdmp_test {

struct Blob {
    std::string d;
    template <class T>
    void put(size_t off, T v) {
        if (d.size() < off + sizeof(T)) d.resize(off + sizeof(T), '\0');
        std::memcpy(&d[off], &v, sizeof(T));
    }
    void put_string(size_t off, const std::string &ascii) {  // MINIDUMP_STRING (UTF-16LE)
        put<uint32_t>(off, (uint32_t)ascii.size() * 2);
        for (size_t i = 0; i < ascii.size(); ++i) put<uint16_t>(off + 4 + 2 * i, (uint16_t)(unsigned char)ascii[i]);
    }
};

constexpr uint64_t kEngine = 0x7ff800000000ULL, kClient = 0x7ff900000000ULL, kLua = 0x7ffa00000000ULL;
constexpr uint64_t kStackStart = 0x0000007f00000000ULL;
constexpr uint32_t kFaultTid = 7;

// stack_words: what sits on the faulting thread's stack from the stack pointer up. via_memory_list: the thread entry has no stack
// of its own and the memory list holds it (a dump with full memory regions).
inline std::string build(const std::vector<uint64_t> &stack_words, bool via_memory_list, bool with_exception = true, uint64_t fault_addr = kEngine + 0x2069e0) {
    Blob b;
    const uint32_t nstreams = with_exception ? 4 : 3;
    b.put<uint32_t>(0, 0x504d444dU);
    b.put<uint32_t>(4, 0xa793);
    b.put<uint32_t>(8, nstreams);
    b.put<uint32_t>(12, 32);
    uint32_t e = 32;
    auto dir = [&](uint32_t type, uint32_t size, uint32_t rva) {
        b.put<uint32_t>(e, type);
        b.put<uint32_t>(e + 4, size);
        b.put<uint32_t>(e + 8, rva);
        e += 12;
    };
    dir(3, 4 + 2 * 48, 0x100);
    dir(4, 4 + 3 * 108, 0x200);
    dir(5, 4 + 16, 0x700);
    if (with_exception) dir(6, 168, 0x400);
    // Thread list: another thread (id 99, a stack the fault is not on), then the faulting one.
    const uint32_t stack_rva = 0x1200;
    b.put<uint32_t>(0x100, 2);
    b.put<uint32_t>(0x104, 99);
    b.put<uint64_t>(0x104 + 24, 0x0000005000000000ULL);
    b.put<uint32_t>(0x104 + 32, 0x100);
    b.put<uint32_t>(0x104 + 36, 0x2000);
    b.put<uint32_t>(0x104 + 48, kFaultTid);
    b.put<uint64_t>(0x104 + 48 + 24, kStackStart);
    b.put<uint32_t>(0x104 + 48 + 32, via_memory_list ? 0 : 0x400);
    b.put<uint32_t>(0x104 + 48 + 36, via_memory_list ? 0 : stack_rva);
    // Modules: full paths, as the engine writes them.
    const std::vector<std::pair<uint64_t, std::string>> mods = {{kEngine, "C:\\Program Files (x86)\\Steam\\steamapps\\common\\GarrysMod\\bin\\win64\\engine.dll"},
                                                                {kClient, "C:\\x\\garrysmod\\bin\\win64\\client.dll"},
                                                                {kLua, "lua_shared.dll"}};
    b.put<uint32_t>(0x200, 3);
    for (size_t i = 0; i < mods.size(); ++i) {
        size_t o = 0x204 + 108 * i;
        b.put<uint64_t>(o, mods[i].first);
        b.put<uint32_t>(o + 8, 0x400000);
        b.put<uint32_t>(o + 20, (uint32_t)(0x800 + 0x100 * i));
        b.put_string(0x800 + 0x100 * i, mods[i].second);
    }
    // Exception: thread, code, address; the CONTEXT with Rsp 0x20 into the stack region.
    b.put<uint32_t>(0x400, kFaultTid);
    b.put<uint32_t>(0x408, 0xC0000005U);
    b.put<uint64_t>(0x408 + 16, fault_addr);
    b.put<uint32_t>(0x400 + 160, 0x4D0);
    b.put<uint32_t>(0x400 + 164, 0x600);
    b.put<uint64_t>(0x600 + 0x98, kStackStart + 0x20);
    b.put<uint64_t>(0x600 + 0xF8, fault_addr);  // Rip, not used
    // Memory list: one 16-byte region far from the stack (and the real one when via_memory_list).
    b.put<uint32_t>(0x700, 1);
    b.put<uint64_t>(0x704, via_memory_list ? kStackStart : 0x0000006000000000ULL);
    b.put<uint32_t>(0x704 + 8, via_memory_list ? 0x400 : 16);
    b.put<uint32_t>(0x704 + 12, via_memory_list ? stack_rva : 0x1100);
    // The stack: a word below the stack pointer that points into a module must not be listed (it is not a caller).
    b.put<uint64_t>(stack_rva + 0x18, kEngine + 0x777);
    for (size_t i = 0; i < stack_words.size(); ++i) b.put<uint64_t>(stack_rva + 0x20 + 8 * i, stack_words[i]);
    if (b.d.size() < stack_rva + 0x400) b.d.resize(stack_rva + 0x400, '\0');
    return b.d;
}


}  // namespace mdmp_test

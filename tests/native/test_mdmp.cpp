// Minidump reader (src/mdmp.cpp), on dumps built by hand following the MINIDUMP_* layouts (real dumps are checked live).
// Issue #8 (live verification of 0.3.7): client_crashed gave only the dump's name; a human (or an agent) wants the faulting module
// and offset and the callers without opening a debugger.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "mdmp.h"
#include "mdmp_builder.h"

using namespace mdmp_test;

void test_mdmp() {
    // The fault is named module+offset (the file name of the module, not its path), the stack words that point into a module are the
    // candidates, in stack order: not the fault address itself, not a module's header, not a repeat, not a word below the stack pointer.
    std::vector<uint64_t> words = {0, kEngine + 0x2069e0, kClient + 0x1234, 0x1234, kClient + 0x1234, kEngine + 0x10, kLua + 0x5555};
    for (int i = 0; i < 10; ++i) words.push_back(kEngine + 0x3000 + 0x10 * i);
    mdmp::Facts f = mdmp::parse(build(words, false));
    CHECK(f.ok && f.fault == "engine.dll+0x2069e0");
    CHECK(f.candidates.size() == 8);  // capped
    CHECK(f.candidates.size() >= 3 && f.candidates[0] == "client.dll+0x1234" && f.candidates[1] == "lua_shared.dll+0x5555" && f.candidates[2] == "engine.dll+0x3000");
    bool bad = false;
    for (const auto &c : f.candidates) bad = bad || c == "engine.dll+0x777" || c == "engine.dll+0x2069e0" || c == "engine.dll+0x10";
    CHECK(!bad);
    CHECK(mdmp::parse(build(words, false), 3).candidates.size() == 3);
    // The same stack held by the memory list instead of the thread entry.
    mdmp::Facts m = mdmp::parse(build(words, true));
    CHECK(m.ok && m.fault == f.fault && m.candidates == f.candidates);
    // Few words: just those.
    mdmp::Facts few = mdmp::parse(build({kClient + 0x1099}, false));
    CHECK(few.ok && few.candidates == std::vector<std::string>{"client.dll+0x1099"});
    // A fault outside every module is a plain address; a stack with nothing in it gives no candidates (the fault is still reported).
    mdmp::Facts out = mdmp::parse(build({1, 2, 3}, false, true, 0x1122334455ULL));
    CHECK(out.ok && out.fault == "0x1122334455" && out.candidates.empty());
    // Damaged or foreign data is not a result, and never a crash.
    CHECK(!mdmp::parse("").ok);
    CHECK(!mdmp::parse("not a dump at all, just text of some length").ok);
    CHECK(!mdmp::parse(build(words, false, false)).ok);  // no exception stream
    std::string whole = build(words, false);
    for (size_t cut : {size_t(20), size_t(40), size_t(0x110), size_t(0x405), size_t(0x610), size_t(0x1300)}) {
        mdmp::Facts t = mdmp::parse(whole.substr(0, cut));
        CHECK(!t.ok || t.candidates.size() <= 8);  // truncated: ok or not, bounded, no read outside the buffer
    }
    std::string wild = whole;
    uint64_t huge = 0xFFFFFFFFFFFFFF00ULL;
    std::memcpy(&wild[0x600 + 0x98], &huge, 8);  // a stack pointer that is in no region
    mdmp::Facts w = mdmp::parse(wild);
    CHECK(w.ok && w.fault == "engine.dll+0x2069e0" && w.candidates.empty());
    CHECK(!mdmp::parse_file("D:/no/such/dump.mdmp").ok);
}

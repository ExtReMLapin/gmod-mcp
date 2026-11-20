// Minidump reader (issue #8, live verification of 0.3.7): what a crash report can say about the dump the game wrote, without a debugger.
// Pure (bytes in, facts out), so it is tested on a dump built in the test; the real dumps of the game are checked live.
//
// Reads the exception stream (faulting thread, exception address and the thread's CONTEXT), the module list and the memory that
// holds the faulting thread's stack, and returns the fault as module+offset and the first return-address candidates of the stack.
// x64 only (Garry's Mod 64-bit); anything else, or a damaged file, gives ok = false.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace mdmp {

struct Facts {
    bool ok = false;                      // the exception record was found and read
    std::string fault;                    // where the exception happened, "engine.dll+0x2069e0" (module unknown: "0x7ff6c0001234")
    std::vector<std::string> candidates;  // values of the faulting thread's stack that point into a module, from the stack pointer up
};

// Candidates are the stack words that fall inside a loaded module, in stack order: not a call stack (no unwinding), a list of places
// to look at; the first ones are usually the callers. At most max_candidates, each address once, within scan_bytes of the stack pointer.
Facts parse(const std::string &bytes, size_t max_candidates = 8, size_t scan_bytes = 16384);

// The file `path` (UTF-8), read up to 32 MB. ok = false when it cannot be read.
Facts parse_file(const std::string &path_u8, size_t max_candidates = 8);

}  // namespace mdmp

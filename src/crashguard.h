// Load crash guard: if the game crashes while the module starts, the next launch skips the module once (no crash
// loop). One marker per process (data/mcpagent/loading/<pid>.txt holding "pid start-stamp"), so that test clients
// started together never mistake each other's load in progress for a crash.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace crashguard {

// Start stamp of a live process whose stamp cannot be read: alive, identity unknown.
inline constexpr uint64_t kStampUnknown = 1;
// Returns the start stamp of a process: 0 = no such process, kStampUnknown = alive but unreadable.
using StampFn = uint64_t (*)(uint32_t pid);

struct Result {
    bool skip = false;               // a previous load crashed: do not start this time
    std::vector<std::string> notes;  // what was found and done, for the log
};

// Called before the module starts. dir = garrysmod/data/mcpagent. Writes this process's marker unless it skips.
Result begin(const std::string &dir, uint32_t pid, StampFn stamp_of);
// Called once the start is over, successful or not (but not crashed): removes this process's marker.
void end(const std::string &dir, uint32_t pid);

}  // namespace crashguard

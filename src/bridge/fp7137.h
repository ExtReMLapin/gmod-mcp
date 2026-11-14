// FP7137 workaround: launch / map_load lock shared by every bridge of this user. REMOVABLE: see docs/FP7137.md.
//
// Engine bug (https://github.com/Facepunch/garrysmod-issues/issues/7137): starting any new gmod_win64.exe makes every
// other running game release and restore its D3D9 resources for 2-10 s, starting ~0.3 s after the process is created.
// A game whose map load ends inside that window crashes in shaderapidx9. So, while the bug lives:
//   - instances_launch waits while any game loads a map (started through a bridge);
//   - map_load waits while a launch is in progress and until `settle_ms` after the last game process was started.
// State is a small file in the bridge's config folder, shared by all bridges and guarded by an OS file lock. Every
// entry names its owner process and carries an expiry, so a dead bridge never leaves a stale lock.
//
// Kill switch: GMOD_MCP_FP7137_WORKAROUND=0 (default on). GMOD_MCP_FP7137_SETTLE_MS overrides the 15 s settle delay
// (tests only).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace fp7137 {

constexpr int64_t kDefaultSettleMs = 15000;

bool enabled();         // FP7137: false when GMOD_MCP_FP7137_WORKAROUND=0
int64_t settle_ms();    // FP7137: settle delay after the last game process start
void set_dir(const std::string &dir);  // FP7137: folder of the shared state (the bridge's config folder)

// --- Pure logic (unit tested) ---

// One thing in progress. kind 'L' = a launch is creating game processes, 'M' = a map load is running.
struct Entry {
    char kind = 'L';
    uint32_t pid = 0;      // the bridge process that owns it
    uint64_t stamp = 0;    // its start stamp (plat::process_start_stamp), 0/1 = unknown: only the pid is checked
    int64_t until_ms = 0;  // wall clock (ms since epoch) after which the entry no longer counts
    std::string tag;       // no whitespace: what it is (map name, "launch")
};
struct State {
    std::vector<Entry> entries;
    int64_t last_start_ms = 0;  // wall clock of the last game process creation by a launch
};

State parse_state(const std::string &text);       // damaged lines are ignored
std::string serialize_state(const State &s);      // "S <last_start_ms>" then "E <kind> <pid> <stamp> <until_ms> <tag>"
// Drops expired entries and the ones whose owner is gone. Returns true if something was dropped.
bool prune(State &s, int64_t now_ms, const std::function<bool(const Entry &)> &owner_alive);

struct Verdict {
    bool wait = false;
    std::string code;         // error code if the wait cannot end in time: launch_in_progress / map_load_in_progress
    std::string reason;       // for the agent
    int64_t ready_at_ms = 0;  // earliest moment it can proceed that is known for sure (0 = unknown)
};
Verdict launch_verdict(const State &s, int64_t now_ms);
Verdict map_load_verdict(const State &s, int64_t now_ms, int64_t settle_ms);
// True when the verdict's known earliest moment already lies beyond the deadline: waiting is pointless.
bool exceeds_deadline(const Verdict &v, int64_t deadline_ms);

// --- Shared lock ---

using Notify = std::function<void(double waited_s, const std::string &reason)>;

enum class Kind { Launch, MapLoad };

// Holds a slot of the shared state for the duration of a launch's process creation or a map load. Destroying it
// releases the slot.
class Gate {
  public:
    Gate() = default;
    Gate(const Gate &) = delete;
    Gate &operator=(const Gate &) = delete;
    ~Gate() { release(); }

    // Waits until the call may go ahead (checked under the shared file lock, then registered), for at most budget_s or
    // until *cancelled. hold_s is how long the slot counts if never released. A wait that cannot end in time fails:
    // ok() is false and code()/message() say why. With the workaround off it returns at once.
    void enter(Kind kind, const std::string &tag, double budget_s, double hold_s, const std::atomic<bool> *cancelled,
               const Notify &notify);
    bool ok() const { return ok_; }
    bool aborted() const { return aborted_; }  // cancelled while waiting
    double waited_s() const { return waited_s_; }
    const std::string &code() const { return code_; }
    const std::string &reason() const { return reason_; }  // why it waited (or could not)
    // A launch calls this right after each CreateProcess: the settle delay counts from here.
    void note_started();
    // Early release (idempotent).
    void release();
    // The slot is released when running() turns false (polled once a second) or at its expiry, on another thread: a
    // map_load run as an MCP task ends long after the call that started it returns.
    void release_when_done(std::function<bool()> running);

    // Adds waited_s / waited_reason to a result's structuredContent (and its text content when it is that JSON).
    void annotate(nlohmann::json &tool_result) const;
    // The tool result for a failed wait.
    nlohmann::json error_result() const;

  private:
    bool entered_ = false, ok_ = true, aborted_ = false;
    double waited_s_ = 0;
    std::string code_, reason_, tag_;
    int64_t until_ms_ = 0;
};

}  // namespace fp7137

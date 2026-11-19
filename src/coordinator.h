// Multi-instance: coordinator (base port) and members (registration, heartbeat, relay, re-election).
#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>


namespace coordinator {

void start();
void stop();
// Member side, when the game shuts down cleanly: tells the coordinator, so that the process going away is not taken for a crash.
void say_goodbye();
bool is_coordinator();
// FP7137 (issue #7): sends a heartbeat now instead of at the next 2 s tick (the "loading" flag just changed). Any thread.
void wake();
// "coordinator", "registered", "refused" (another token), "foreign" (the base port is held by a program that is not
// gmod-mcp) or "unreachable".
std::string link_state();
// Error text for a call to an instance that stopped answering: says whether its process is still alive.
std::string gone_message(const std::string &id, uint32_t pid, double seconds_ago, bool process_alive);
// Pure: is the process a member was registered with still the one running under that pid? `now` is
// plat::process_start_stamp(pid) taken now (0 = no such process, 1 = exists but unreadable), `recorded` the stamp taken
// while the member was alive (0 = never recorded). A different stamp means the pid was reused by an unrelated process.
bool same_process_running(uint64_t now, uint64_t recorded);
nlohmann::json instances();  // for instances_list
// Id of the instance serving as coordinator (this one, or the one that answered the last heartbeat; "" if unknown).
std::string coordinator_id();
// True for kHandoverGraceS seconds after this instance took the base port over from a coordinator that went away: an
// instance it does not know yet may just not have re-registered (calls to it answer coordinator_changing).
bool in_handover();
// instances_list: {state, retry_after_s, note} during a hand-over, else null.
nlohmann::json handover_info();
// For game_quit / game_terminate on this instance: when it is the coordinator and other instances live, {coordinator_moved,
// remaining_instances, warning} to add to the answer, else null.
nlohmann::json leaving_note();
// Tests only: starts (true) or ends (false) the hand-over grace period without an election.
void note_takeover_for_tests(bool on);
// Tests only: shortens the waits (seconds; a value <= 0 keeps the default): no heartbeat before an instance leaves the list,
// main thread blocked before it is `hung`, heartbeat silence that aborts a call in flight, low fps before `fps_low_s` shows.
void set_timing_for_tests(double dead_after_s, double hung_after_s, double inflight_silent_s, double low_fps_shown_s);

}  // namespace coordinator

// Multi-instance: coordinator (base port) and members (registration, heartbeat, relay, re-election).
#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace coordinator {

void start();
void stop();
bool is_coordinator();
// "coordinator", "registered", "refused" (another token) or "unreachable".
std::string link_state();
// Error text for a call to an instance that stopped answering: says whether its process is still alive.
std::string gone_message(const std::string &id, uint32_t pid, double seconds_ago, bool process_alive);
// Pure: is the process a member was registered with still the one running under that pid? `now` is
// plat::process_start_stamp(pid) taken now (0 = no such process, 1 = exists but unreadable), `recorded` the stamp taken
// while the member was alive (0 = never recorded). A different stamp means the pid was reused by an unrelated process.
bool same_process_running(uint64_t now, uint64_t recorded);
nlohmann::json instances();  // for instances_list

}  // namespace coordinator

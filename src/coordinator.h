// Multi-instance: coordinator (base port) and members (registration, heartbeat, relay, re-election).
#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace coordinator {

void start();
void stop();
bool is_coordinator();
// Error text for a call to an instance that stopped answering: says whether its process is still alive.
std::string gone_message(const std::string &id, uint32_t pid, double seconds_ago, bool process_alive);
nlohmann::json instances();  // for instances_list

}  // namespace coordinator

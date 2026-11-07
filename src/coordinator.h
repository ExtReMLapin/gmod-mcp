// Multi-instance: coordinator (base port) and members (registration, heartbeat, relay, re-election).
#pragma once

#include <nlohmann/json.hpp>

namespace coordinator {

void start();
void stop();
bool is_coordinator();
nlohmann::json instances();  // for instances_list

}  // namespace coordinator

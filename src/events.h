// Typed event stream (monotonic cursor) + blocking wait (long-poll) + report channels.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace events {

void publish(const std::string &type, const nlohmann::json &data);
uint64_t cursor_now();
// Returns events with sequence >= cursor whose type is in `types` (empty = all). Waits up to timeout_s
// if none match. Result: {events, next_cursor, lost, timed_out}.
nlohmann::json wait(const std::vector<std::string> &types, uint64_t cursor, double timeout_s, size_t limit);

// Report channels (mcpagent.report on the Lua side).
void report(const std::string &channel, const std::string &realm, const nlohmann::json &value);
nlohmann::json reports_read(const std::string &channel, uint64_t cursor, size_t limit, double wait_s);
nlohmann::json reports_channels();
void reports_clear(const std::string &channel);  // "" = all

// Watchdog: thread that publishes main_thread_stalled / main_thread_resumed.
void start_watchdog(double stall_s);
void stop_watchdog();

}  // namespace events

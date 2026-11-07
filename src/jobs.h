// Job queue: the network thread pushes, the main thread pops every frame.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>

#include <nlohmann/json.hpp>

namespace jobs {

using Fn = std::function<nlohmann::json()>;

// Runs fn on the main thread and waits for the result for at most timeout_s seconds.
// On timeout: returns {"__timeout": true} and the job is cancelled if it has not started yet.
nlohmann::json run_on_main(Fn fn, double timeout_s);
bool is_timeout(const nlohmann::json &r);

// Called by the pump (main thread): runs pending jobs, within a budget of max_ms.
void pump(double max_ms);
size_t pending();
// Cancels all pending jobs (module shutdown).
void cancel_all();

// Frame counter and timestamp of the last tick (watchdog).
void on_frame();
uint64_t frame_count();
double seconds_since_last_frame();
// Frame statistics (fps, average/max frametime over the sliding window).
nlohmann::json frame_stats();

}  // namespace jobs

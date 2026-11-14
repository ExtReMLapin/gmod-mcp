// Captures the console (tier0 spew, console.log fallback) and Lua errors, into ring buffers with a monotonic cursor.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace console {

struct Line {
    uint64_t seq;
    double time;  // seconds since module startup
    std::string level;  // message | warning | assert | error | log
    std::string text;
};

// Installs the capture. Returns the method used: "spew" (tier0 SpewOutputFunc), "logging" (tier0 LoggingSystem
// listener, newer tier0 such as the x86-64 Linux dedicated server), "condebug" (console.log tail) or "none".
std::string install(const std::string &condebug_path);
void uninstall();
// Called every frame: checks that the spew hook is still in place.
void maintain();
std::string method();

void push_line(const std::string &level, const std::string &text);

// Called for every complete console line (any thread, outside the console lock).
using LineObserver = std::function<void(const std::string &)>;
void set_line_observer(LineObserver fn);

// Read: returns lines with sequence >= cursor (at most `limit`), the new cursor, and the number of lost lines.
struct ReadResult {
    std::vector<Line> lines;
    uint64_t next_cursor;
    uint64_t lost;
    uint64_t oldest;
};
ReadResult read(uint64_t cursor, size_t limit, const std::string &regex, std::string &err);
uint64_t cursor_now();
void set_capacity(size_t n);
size_t capacity();

// Lua errors.
void push_error(const nlohmann::json &e);  // e contains realm, message, stack...; tagged with map_load (below)
// Source of the current map-load sequence number, stamped on each error as "map_load" (set by the agent).
void set_map_load_source(uint64_t (*fn)());
nlohmann::json errors_since(uint64_t cursor, size_t limit);
uint64_t errors_cursor();
void errors_clear();
// Parses console lines to extract errors from them (srcds mode, without OnLuaError).
void set_parse_errors_from_console(bool on);

double uptime();

}  // namespace console

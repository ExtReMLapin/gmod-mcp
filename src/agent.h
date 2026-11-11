// Global agent state: mode, main-thread pump, runtime injection, client network lock.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

struct lua_State;

namespace agent {

enum class Mode { Client, Dedicated };

struct Config {
    Mode mode = Mode::Client;
    int base_port = 27080;
    int port = 0;  // port actually obtained
    std::string token;
    std::string game_dir;  // .../garrysmod (absolute)
    std::string tmp_dir;   // .../garrysmod/lua/mcpagent_tmp (absolute)
    std::string data_dir;  // .../garrysmod/data/mcpagent (absolute)
    std::string instance_id;
    std::string version = GMOD_MCP_VERSION;
};

Config &config();
const char *mode_name();

// Main thread only.
void pump(lua_State *host);
lua_State *host_state();  // menu (client) or server (srcds)
int host_realm();

// Network lock: returns true if operations on the client realm are allowed. Main thread.
bool client_allowed(std::string *reason = nullptr);
// Last computed state (readable from any thread).
nlohmann::json lock_status();

// Calls a realm's Lua runtime (main thread). Injects it if needed. Returns {"ok":..,"result"|"error"}.
nlohmann::json call_runtime(int realm, const std::string &tool, const nlohmann::json &args);
// From the network thread: same, via the job queue with a timeout.
nlohmann::json rt(int realm, const std::string &tool, const nlohmann::json &args, double timeout_s = 10.0);

// Injection generation per realm (incremented on each new injection = new state).
uint64_t generation(int realm);
// Realm availability (last state seen by the pump, readable from any thread).
bool realm_present(int realm);
bool realm_injected(int realm);
// Realm ready (gamemode loaded, local player valid on the client side) for its current generation.
bool realm_ready(int realm);
// Map currently loaded ("" at the main menu), updated on map_loaded / disconnected.
std::string current_map();
// Queues a one-time notice shown in game to the local player once the client realm is ready (client mode only).
void notify_player(const std::string &text);

// lua_job: marks a realm as having running jobs, so that the pump resumes them every frame.
void jobs_wake(int realm);

// Structured error code.
nlohmann::json err(const std::string &code, const std::string &message, nlohmann::json extra = nullptr);

}  // namespace agent

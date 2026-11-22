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
    bool test_client = false;  // -mcp_test_client: started by the bridge (instances_launch); see native.test_client
    std::string owner;  // -mcp_owner (issue #8): the instances_launch owner label, carried in the heartbeat so it outlives the launching bridge
    std::string restore_convars;  // -mcp_restore_convars a,b (Issue #10): more archived convars to give back on a clean quit; see native.restore_convars
    unsigned long long return_focus = 0;  // -mcp_return_focus <hwnd> (test clients): the window that gets the foreground back; see launchopts::decide_return_focus
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

// Issue #30: may a lua_job in realm `from` run code in realm `to`? Empty when yes, else the reason. `depth` is how many
// cross-realm calls are already in progress (one level only: the other realm's chunk is not a job and has no `job`).
std::string cross_call_refusal(int from, int to, int depth);
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
// Map loads: counter incremented when a new server realm comes up (a map starts loading), with the map name if known.
// Lua errors are tagged with it, so errors_since can keep only the current map's.
void note_map_load_start(const std::string &map);
uint64_t map_load_seq();
// A map load starts when a NEW server Lua state appears. The module learns it as early as it can: gmod13_open runs in
// every new server state (the autorun loader requires the module), in client and srcds modes alike, before the map's
// other autorun files when it can; gmod13_close runs in the old one. The pump seeing the server realm come up
// (server_realm_appeared) is the fallback when the loader is missing, and never counts a load twice. Errors raised
// between the close of the old server state and the open of the new one (shared autorun files, autorun files sorted
// before the loader) belong to the new load: on_server_state_open re-stamps them.
void on_server_state_open();
void on_server_state_close();
void server_realm_appeared();
// Map of a given map load ("" if not known yet).
std::string map_of_load(uint64_t seq);
// FP7137 (issue #7): is a map load in progress, whatever started it (console, Lua, map_load)? Readable from any thread.
// since_s (optional) gets the seconds since the oldest load that still counts. Stops counting a load after 300 s.
bool map_loading(double *since_s = nullptr);
// Live verification of 0.3.7: a map_load this module executes counts as a load from its first moment (it waits up to 15 s for the menu,
// then 30 frames, before the engine starts the load, and `loading` said false meanwhile). RAII in the map_load tool.
struct MapLoadCall {
    MapLoadCall();
    ~MapLoadCall();
    MapLoadCall(const MapLoadCall &) = delete;
    MapLoadCall &operator=(const MapLoadCall &) = delete;
};
// Issue #8 (live check I-C1): the engine reports a loading screen with nothing behind it (no realm, not connected, no load starting) for
// longer than a few seconds: its state is stuck; that screen no longer counts as a load. A map_load clears it; `disconnect` does not (checked live).
bool map_loading_stuck();
// What status and instances_list say about it.
inline const char *loading_stuck_note() {
    return "the engine reports a loading screen (the menu's IsInLoading) but nothing is loading: no server or client realm, not connected, "
           "no load in progress. It is a stuck engine state (typically after `map` then `disconnect` within a second), not a load, so `loading` is "
           "false and launches are not held back. map_load clears it; `disconnect` does not.";
}
// Queues a one-time notice shown in game to the local player once the client realm is ready (client mode only).
void notify_player(const std::string &text);

// lua_job: marks a realm as having running jobs, so that the pump resumes them every frame.
void jobs_wake(int realm);

// Structured error code.
nlohmann::json err(const std::string &code, const std::string &message, nlohmann::json extra = nullptr);
// The `realm_unavailable` error of a realm that cannot be used now. On a dedicated server (srcds) the answer says so and carries
// details {mode: "srcds", realm}: the menu and client realms never exist there ("use 'server'"), and a missing server realm is the
// gap between two maps. In client mode: "realm X unavailable (not loaded)", the map is not loaded yet.
nlohmann::json realm_unavailable_err(int realm);

}  // namespace agent

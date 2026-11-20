// Tool: instances_list (multi-instance coordination).
#include "coordinator.h"
#include "mcp.h"
#include "tools.h"

namespace tools {

void register_instances() {
    mcp::Tool t{"instances_list", "List instances",
                "Known gmod-mcp instances (this one, and on the coordinator every registered member): id, mode (client/srcds), "
                // FP7137 (issue #7, see docs/FP7137.md): the `loading` words of the next line
                "port, pid, map, realms (client true = the client is in game), role, heartbeat age and health: agent_version, uptime_s, "
                "fps (over the last 3 s), main_thread_stalled, stalled_s (seconds the main thread has been blocked, 0 when it ticks), fps_low_s (seconds the frame rate has stayed under 10 fps, 0 when fine or while the game loads: measurements taken meanwhile are suspect), loading (true while a map load is in progress, whatever started it: console, Lua, map_load; sent at once when it starts and ends; it ends when the server state that started it is gone, e.g. a `disconnect` right after `map`, and a load whose loading screen has been down for 30 s no longer counts; a loading screen with nothing behind it for 12 s (no server or client realm, not connected, no load starting: the engine's stuck state after `map` then `disconnect` at once) is not a load: `loading` is false and the entry carries loading_stuck: true, and map_load clears it (`disconnect` does not); status reports the same flags), lua_errors (count since start; errors_since for the details). The instance with role \"coordinator\" (also named by coordinator_id) holds the directory: when it ends, another takes over within about a second, and during the hand-over `coordinator_changing: true` (calls with `instance` answer the retryable error coordinator_changing, with retry_after_s). Instances whose process ended are removed, except those that crashed: they stay an hour with state \"crashed\" and crash {dump (crashes/*.mdmp file name or null), exit_code, evidence}; relaunch with instances_launch, the dump is for a human. States: \"running\" (live); \"hung\" (main thread blocked for 60 s, 180 s during a map load, or no heartbeat for that long while the process lives: a load that never ends, an infinite loop, a crash dialog; it will not recover: the `note` says to close it with instances_close {pids} or game_terminate {instance} and relaunch, and what was measured on it since is void); \"unresponsive\" (no heartbeat for 10 s but the process lives: busy or frozen, becomes hung after the limit); \"crashed\". A call in flight to an instance that dies is answered at once with client_crashed / server_crashed, and one whose process is frozen with no heartbeat for 30 s with instance_hung. Any tool accepts "
                "an `instance` argument: the coordinator relays the call to that instance. Through the plugin's bridge, "
                "entries also carry the `owner` label given to instances_launch (sent by the game itself, so it shows for every instance "
                "whichever bridge session launched it) and `test_client` (true for clients started by instances_launch), `owner` filters on it, and "
                "launched_not_answering lists the clients it launched that never answered (close them with "
                "instances_close {pids}); with no game running the list is empty, with a note.",
                json{{"properties", {{"owner", {{"type", "string"}, {"maxLength", 64},
                                                 {"description", "Only the clients launched with this owner label (bridge only)."}}}}}},
                json{{"readOnlyHint", true}},
                [](const json &) {
                    // `coordinator`: this instance is the coordinator; `coordinator_id`: which instance is (the entry with role
                    // "coordinator" is the only one that holds the directory: closing it moves the role, issue #6).
                    json r{{"instances", coordinator::instances()}, {"coordinator", coordinator::is_coordinator()},
                           {"coordinator_id", coordinator::coordinator_id()}};
                    json h = coordinator::handover_info();
                    if (!h.is_null()) {
                        r["coordinator_changing"] = true;
                        r["handover"] = h;
                    }
                    return ok(r);
                }};
    t.local_only = true;
    mcp::register_tool(t);
}

}  // namespace tools

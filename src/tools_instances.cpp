// Tool: instances_list (multi-instance coordination).
#include "coordinator.h"
#include "mcp.h"
#include "tools.h"

namespace tools {

void register_instances() {
    mcp::Tool t{"instances_list", "List instances",
                "Known gmod-mcp instances (this one, and on the coordinator every registered member): id, mode (client/srcds), "
                "port, pid, map, realms (client true = the client is in game), role, heartbeat age and health: agent_version, uptime_s, "
                "fps, main_thread_stalled, lua_errors (count since start; errors_since for the details). Dead instances (no heartbeat for 10 s) are removed. Any tool accepts "
                "an `instance` argument: the coordinator relays the call to that instance. Through the plugin's bridge, "
                "entries also carry the `owner` label given to instances_launch, `owner` filters on it, and "
                "launched_not_answering lists the clients it launched that never answered (close them with "
                "instances_close {pids}); with no game running the list is empty, with a note.",
                json{{"properties", {{"owner", {{"type", "string"}, {"maxLength", 64},
                                                 {"description", "Only the clients launched with this owner label (bridge only)."}}}}}},
                json{{"readOnlyHint", true}},
                [](const json &) { return ok(json{{"instances", coordinator::instances()}, {"coordinator", coordinator::is_coordinator()}}); }};
    t.local_only = true;
    mcp::register_tool(t);
}

}  // namespace tools

// Tools: event stream (long-poll) and report channels.
#include "events.h"
#include "mcp.h"
#include "tools.h"

namespace tools {

static json events_wait_tool(const json &args) {
    std::vector<std::string> types;
    if (args.contains("types"))
        for (const auto &t : args["types"]) types.push_back(t.get<std::string>());
    uint64_t cursor = args.contains("cursor") ? args["cursor"].get<uint64_t>() : events::cursor_now();
    double to = args.value("timeout_s", 30.0);
    size_t limit = args.value("limit", 100);
    return ok(events::wait(types, cursor, to, limit));
}

static json reports_read_tool(const json &args) {
    return ok(events::reports_read(args["channel"], args.value("cursor", (uint64_t)0), args.value("limit", 500),
                                   args.value("wait_s", 0.0)));
}

void register_reports() {
    json RO = {{"readOnlyHint", true}};
    mcp::register_tool(
        {"events_wait", "Wait for events",
         "Long-poll on the instance's event stream: returns AS SOON AS a matching event arrives, otherwise after "
         "timeout_s (timed_out=true). Types: menu_ready, runtime_injected, realm_up, realm_down, realm_ready, map_loaded, "
         "disconnected, disconnect_reason, connection_failed, main_thread_stalled, main_thread_resumed, lua_error, runtime_error, report, user:<type> (mcpagent.event). Without `cursor`, "
         "only waits for future events; reuse next_cursor to chain calls without loss (lost > 0 on overflow).",
         json{{"properties",
               {{"types", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Types to wait for (empty = all)."}}},
                {"cursor", {{"type", "integer"}, {"minimum", 0}, {"description", "Cursor (next_cursor from a previous call)."}}},
                {"timeout_s", {{"type", "number"}, {"minimum", 0}, {"maximum", 600}, {"description", "Max wait (default 30)."}}},
                {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}, {"description", "Max number of events (default 100)."}}}}}},
         RO, events_wait_tool});
    mcp::register_tool(
        {"reports_read", "Read a report channel",
         "Reads values published by scripts via mcpagent.report(channel, value) (available in all realms). "
         "Incremental read by cursor; wait_s > 0 = long-poll (returns as soon as a new value arrives). "
         "Ideal for long scans/measurements started by lua_exec_file.",
         json{{"properties",
               {{"channel", {{"type", "string"}, {"description", "Channel name."}}},
                {"cursor", {{"type", "integer"}, {"minimum", 0}, {"description", "Cursor (default 0 = from the start of the buffer)."}}},
                {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10000}, {"description", "Default 500."}}},
                {"wait_s", {{"type", "number"}, {"minimum", 0}, {"maximum", 600}, {"description", "Max wait if nothing new (default 0)."}}}}},
              {"required", {"channel"}}},
         RO, reports_read_tool});
    mcp::register_tool({"reports_channels", "List report channels",
                        "Existing channels with the number of buffered values, the total published and the next cursor.", json::object(),
                        RO, [](const json &) { return ok(json{{"channels", events::reports_channels()}}); }});
    mcp::register_tool({"reports_clear", "Clear report channels", "Deletes a channel (or all of them if `channel` is absent).",
                        json{{"properties", {{"channel", {{"type", "string"}, {"description", "Channel to clear (absent = all)."}}}}}},
                        json{{"destructiveHint", true}, {"idempotentHint", true}}, [](const json &a) {
                            events::reports_clear(a.value("channel", ""));
                            return ok(json{{"cleared", a.value("channel", "*")}});
                        }});
}

}  // namespace tools

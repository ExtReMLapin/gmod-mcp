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
         "Waits for events of this game instance and returns AS SOON AS one of the wanted types arrives (all that arrived, "
         "up to limit), or after timeout_s with timed_out: true. Types: menu_ready, runtime_injected, realm_up, realm_down, "
         "realm_ready, map_loaded, disconnected, disconnect_reason, connection_failed, main_thread_stalled, "
         "main_thread_resumed, lua_error, runtime_error, report (a mcpagent.report value), focus_released, user:<type> "
         "(sent by game code with mcpagent.event(type, data)). Without `cursor` it waits for events from now on; pass the "
         "returned next_cursor to the next call so nothing is missed in between. The instance keeps the last 5000 events "
         "(lost > 0 = some were dropped before you read them). Result: {events: [{seq, time (s since module start), type, "
         "data}], next_cursor, lost, timed_out}. Example: {\"types\": [\"map_loaded\", \"lua_error\"], \"timeout_s\": 60}.",
         json{{"properties",
               {{"types", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Types to wait for (empty = all)."}}},
                {"cursor", {{"type", "integer"}, {"minimum", 0}, {"description", "Cursor (next_cursor from a previous call)."}}},
                {"timeout_s", {{"type", "number"}, {"minimum", 0}, {"maximum", 600}, {"description", "Max wait (default 30)."}}},
                {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}, {"description", "Max number of events (default 100)."}}}}}},
         RO, events_wait_tool});
    mcp::register_tool(
        {"reports_read", "Read a report channel",
         "Reads the values that game code published with mcpagent.report(channel, value) (a Lua function gmod-mcp adds in "
         "every realm; job:report(value) of a lua_job publishes on \"job:<id>\"). Values come back whole, as JSON. Read "
         "incrementally: cursor 0 = the oldest kept value, then pass next_cursor. wait_s > 0 waits until a new value "
         "arrives (or wait_s passes). Each channel keeps its last 10000 values (lost > 0 = dropped before you read them); "
         "channels survive map changes and live until reports_clear or the game exits. Result: {channel, items: [{seq, "
         "time, realm, value}], next_cursor, lost, timed_out}. Example: in the game `mcpagent.report(\"scan\", {map = "
         "game.GetMap(), hits = n})`, then {\"channel\": \"scan\", \"cursor\": 0, \"wait_s\": 30}.",
         json{{"properties",
               {{"channel", {{"type", "string"}, {"description", "Channel name."}}},
                {"cursor", {{"type", "integer"}, {"minimum", 0}, {"description", "Cursor (default 0 = from the start of the buffer)."}}},
                {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10000}, {"description", "Default 500."}}},
                {"wait_s", {{"type", "number"}, {"minimum", 0}, {"maximum", 600}, {"description", "Max wait if nothing new (default 0)."}}}}},
              {"required", {"channel"}}},
         RO, reports_read_tool});
    mcp::register_tool({"reports_channels", "List report channels",
                        "Report channels of this instance: {channels: [{channel, buffered (values kept), total (ever published), "
                        "next_cursor}]}.", json::object(),
                        RO, [](const json &) { return ok(json{{"channels", events::reports_channels()}}); }});
    mcp::register_tool({"reports_clear", "Clear report channels", "Deletes a report channel and its values (all channels if `channel` is absent). Result: {cleared}.",
                        json{{"properties", {{"channel", {{"type", "string"}, {"description", "Channel to clear (absent = all)."}}}}}},
                        json{{"destructiveHint", true}, {"idempotentHint", true}}, [](const json &a) {
                            events::reports_clear(a.value("channel", ""));
                            return ok(json{{"cleared", a.value("channel", "*")}});
                        }});
}

}  // namespace tools

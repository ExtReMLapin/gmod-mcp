// MCP layer: JSON-RPC (initialize, ping, tools/*, resources/*), tool registry, input validation.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <memory>

#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

namespace mcp {

using json = nlohmann::json;

// A handler returns {"ok":true,"result":{...}} or {"ok":false,"error":{code,message,details?}}.
// It may add "image": {"data": base64, "mimeType": "..."} to return an MCP image.
using Handler = std::function<json(const json &args)>;

struct Tool {
    std::string name;
    std::string title;
    std::string description;
    json input_schema;  // {"type":"object","properties":...,"required":[...]} - additionalProperties forced to false
    json annotations;   // readOnlyHint, destructiveHint, idempotentHint, openWorldHint
    Handler handler;
    bool local_only = false;  // no multi-instance relay (e.g. instances_list)
    std::string task_support;  // "" (= forbidden), "optional" or "required" (MCP Tasks)
    std::shared_ptr<nlohmann::json_schema::json_validator> validator;  // compiled at registration
    json output_schema;  // structuredContent schema (a permissive default is set at registration)
};

void register_tool(Tool t);
// Declares a tool as runnable as an MCP task ("optional").
void set_task_support(const std::string &name, const std::string &mode);
const std::vector<Tool> &tools();
const Tool *find_tool(const std::string &name);

// Relevant HTTP headers (lowercase names).
using Headers = std::map<std::string, std::string>;
// Handles an HTTP body (one JSON-RPC message). Returns the response body, or "" for a notification (HTTP 202).
std::string handle(const std::string &body, const Headers &headers, int &http_status);

// Two-phase handling, used by the HTTP layer to answer on an SSE stream when useful:
// prepare() parses and validates the headers; it returns null when the answer is already known (`immediate`,
// possibly empty for a notification, with `http_status`).
struct Prepared;
std::shared_ptr<Prepared> prepare(const std::string &body, const Headers &headers, std::string &immediate, int &http_status);
// True if the request benefits from an SSE response (progress, cancellation by closing the stream, subscriptions).
bool wants_stream(const Prepared &p);
// Sends one JSON-RPC message (notification) on the response stream.
using Emit = std::function<void(const json &message)>;
// Executes the request. emit is null for a plain JSON response. `cancel` is set by the transport when the client
// goes away. Returns the final JSON-RPC response body ("" for a notification).
std::string execute(Prepared &p, int &http_status, const Emit &emit, std::shared_ptr<std::atomic<bool>> cancel);

// Direct tool call (used by the relay and internal tests); returns the MCP CallToolResult.
json call_tool(const std::string &name, const json &args);

// Validates a value against a JSON schema (json-schema-validator). Returns "" if OK, otherwise the error message.
std::string validate(const json &schema, const json &value, const std::string &path = "");

// Multi-instance relay: if set, called when `instance` designates another instance.
using RelayFn = std::function<json(const std::string &instance, const std::string &tool, const json &args)>;
void set_relay(RelayFn fn);
// Other live instances registered with this coordinator (empty when alone or not coordinator).
using PeersFn = std::function<std::vector<std::string>()>;
void set_peers(PeersFn fn);

void set_instructions(std::string s);
extern const char *kProtocolVersion;

}  // namespace mcp

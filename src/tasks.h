// MCP Tasks, two generations over the same store:
//  - "legacy": experimental Tasks from the 2025-11-25 spec (params.task, CreateTaskResult {task}, blocking tasks/result);
//  - "modern": io.modelcontextprotocol/tasks extension (2026-07-28 spec: result {resultType:"task", ...},
//              final result included in tasks/get, tasks/update, tasks/cancel = empty acknowledgement).
#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace tasks {

using json = nlohmann::json;

enum class Era { Legacy, Modern };

extern const char *kExtensionId;  // "io.modelcontextprotocol/tasks"

// 2025-11-25 server capability (capabilities.tasks field of initialize).
json legacy_capability();

// Creates a task for a tools/call. Returns the creation result in the era's format, or {"__rpc_error": {...}}.
json create_tool_task(const std::string &tool, const json &args, const json &task_params, Era era);

// JSON-RPC methods; return the "result" or {"__rpc_error": {...}}.
json get(const json &params, Era era);
json result(const json &params);  // legacy only (blocking)
json list(const json &params);    // legacy only
json cancel(const json &params, Era era);
json update(const json &params);  // modern only (no task awaits input: acknowledgement)

// Cooperative cancellation: true if the current task (calling thread) has been cancelled.
bool should_stop();

}  // namespace tasks

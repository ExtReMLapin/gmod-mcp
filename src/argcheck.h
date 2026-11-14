// Checks tool arguments against a tool's inputSchema (the subset gmod-mcp's schemas use). Pure; used by the bridge for
// its own tools (the module validates its tools with a full JSON Schema validator).
#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace argcheck {

// "" if `args` fits `schema` (an object schema: properties with type, enum, minimum, maximum, minLength, maxLength,
// pattern, items, maxItems, minItems; additionalProperties false; required), else a message naming the argument.
std::string check(const nlohmann::json &schema, const nlohmann::json &args);

}  // namespace argcheck

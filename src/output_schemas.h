#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace mcp {

// The declared outputSchema of a tool's success result (see output_schemas.cpp), or null when the tool has none yet.
nlohmann::json output_schema_for(const std::string &tool);

}  // namespace mcp

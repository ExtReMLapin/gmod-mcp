// MCP prompts (shown as slash commands by clients such as Claude Code) and their argument completion.
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace prompts {

nlohmann::json list();
// GetPromptResult, or null with `err` set (unknown prompt or missing argument).
nlohmann::json get(const std::string &name, const nlohmann::json &args, std::string &err);
std::vector<std::string> complete(const std::string &prompt, const std::string &arg, const std::string &value);

}  // namespace prompts

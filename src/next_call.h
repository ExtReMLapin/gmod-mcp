#pragma once

#include <string>

#include <nlohmann/json.hpp>

// Issue #3: a hint that names a next call is also given as data, next to the prose: `next_call` = {tool, arguments?,
// after_s?} (after_s: wait this long first). Header-only helpers shared by the module and the bridge.
namespace next_call {

using json = nlohmann::json;

inline json make(const std::string &tool, json arguments = nullptr, double after_s = 0) {
    json n{{"tool", tool}};
    if (arguments.is_object() && !arguments.empty()) n["arguments"] = std::move(arguments);
    if (after_s > 0) n["after_s"] = after_s;
    return n;
}

// {"instance": <the failed call's>} when the call had one, else {}.
inline json instance_of(const json &args) {
    json a = json::object();
    if (args.is_object() && args.contains("instance") && args["instance"].is_string()) a["instance"] = args["instance"];
    return a;
}

// Puts `n` on a CallToolResult (error.next_call for an error, next_call at the top otherwise) and rewrites the first text
// block so that it stays the JSON of structuredContent. Does nothing for a null `n` or a result without structuredContent.
void attach(json &call_result, const json &n);

// The module's result of a call (CallToolResult: content, structuredContent, isError) gets `next_call` where the tool's
// failure or answer has an obvious next step: error.next_call for an error, next_call at the top for a success. The text
// block is rewritten so that it stays the JSON of structuredContent. Does nothing when there is no next step.
void annotate(json &call_result, const std::string &tool, const json &args);

}  // namespace next_call

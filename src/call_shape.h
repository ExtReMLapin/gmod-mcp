#pragma once
// Issue #18 item 1: the call shape of a tool, `name(required, optional?)`, built from its input schema and put at the
// start of its description when the tool list is served. The tools are often deferred in the agent's client, so the agent
// sees the description before the schema: this is where it learns that `tests_run` wants `path` (not `file`) before the
// first call fails. Header-only: the module and the bridge both use it.
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace call_shape {

// Required arguments first (schema order), then the optional ones with a `?`: the ones an agent reaches for first (realm, code,
// path, name, timeout_s) when present, then the rest alphabetically (the schema's object order), at most kMaxOptional of them,
// then `...`. `instance` is on every tool and never listed.
inline std::string of(const std::string &name, const nlohmann::json &input_schema) {
    constexpr size_t kMaxOptional = 8;
    static const char *const kPriority[] = {"realm", "code", "path", "name", "timeout_s"};
    std::string req;
    std::vector<std::string> optional;
    nlohmann::json required = input_schema.value("required", nlohmann::json::array());
    auto is_required = [&](const std::string &k) {
        for (const auto &r : required)
            if (r.is_string() && r.get<std::string>() == k) return true;
        return false;
    };
    if (input_schema.contains("properties") && input_schema["properties"].is_object()) {
        for (const auto &r : required) {
            if (!r.is_string() || r.get<std::string>() == "instance") continue;
            if (!req.empty()) req += ", ";
            req += r.get<std::string>();
        }
        const auto &props = input_schema["properties"];
        for (const char *p : kPriority)
            if (props.contains(p) && !is_required(p)) optional.push_back(p);
        for (auto it = props.begin(); it != props.end(); ++it) {
            if (it.key() == "instance" || is_required(it.key())) continue;
            bool done = false;
            for (const char *p : kPriority) done = done || it.key() == p;
            if (!done) optional.push_back(it.key());
        }
    }
    std::string args = req;
    for (size_t i = 0; i < optional.size() && i < kMaxOptional; ++i) args += (args.empty() ? "" : ", ") + optional[i] + "?";
    if (optional.size() > kMaxOptional) args += ", ...";
    return name + "(" + args + ")";
}

// "name(args). description", unless the description already starts with the shape (idempotent).
inline std::string prefixed(const std::string &name, const nlohmann::json &input_schema, const std::string &description) {
    std::string shape = of(name, input_schema);
    if (description.rfind(shape, 0) == 0) return description;
    return shape + ". " + description;
}

}  // namespace call_shape

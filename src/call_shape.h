#pragma once
// Issue #18 item 1: the call shape of a tool, `name(required, optional?)`, built from its input schema and put at the
// start of its description when the tool list is served. The tools are often deferred in the agent's client, so the agent
// sees the description before the schema: this is where it learns that `tests_run` wants `path` (not `file`) before the
// first call fails. Header-only: the module and the bridge both use it.
#include <string>

#include <nlohmann/json.hpp>

namespace call_shape {

// Required arguments first (schema order), then the optional ones with a `?`, at most kMaxOptional of them, then `...`.
// `instance` is on every tool and never listed. Alphabetical inside each group (the schema's object order).
inline std::string of(const std::string &name, const nlohmann::json &input_schema) {
    constexpr size_t kMaxOptional = 6;
    std::string req, opt;
    size_t n_opt = 0, shown = 0;
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
        for (auto it = input_schema["properties"].begin(); it != input_schema["properties"].end(); ++it) {
            if (it.key() == "instance" || is_required(it.key())) continue;
            ++n_opt;
            if (shown >= kMaxOptional) continue;
            ++shown;
            if (!opt.empty()) opt += ", ";
            opt += it.key() + "?";
        }
    }
    std::string args = req;
    if (!opt.empty()) args += (args.empty() ? "" : ", ") + opt;
    if (n_opt > shown) args += ", ...";
    return name + "(" + args + ")";
}

// "name(args). description", unless the description already starts with the shape (idempotent).
inline std::string prefixed(const std::string &name, const nlohmann::json &input_schema, const std::string &description) {
    std::string shape = of(name, input_schema);
    if (description.rfind(shape, 0) == 0) return description;
    return shape + ". " + description;
}

}  // namespace call_shape

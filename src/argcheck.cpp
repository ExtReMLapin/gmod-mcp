#include "argcheck.h"

#include <regex>

namespace argcheck {

using nlohmann::json;

static bool type_is(const json &v, const std::string &t) {
    if (t == "string") return v.is_string();
    if (t == "boolean") return v.is_boolean();
    if (t == "object") return v.is_object();
    if (t == "array") return v.is_array();
    if (t == "null") return v.is_null();
    if (t == "number") return v.is_number();
    if (t == "integer") {
        if (v.is_number_integer()) return true;
        if (!v.is_number_float()) return false;
        double d = v.get<double>();
        return d == (double)(long long)d;  // 1.0 is an integer in JSON Schema
    }
    return true;  // unknown type keyword: do not refuse
}

static std::string check_value(const json &s, const json &v, const std::string &name) {
    if (!s.is_object()) return "";
    if (s.contains("type")) {
        bool any = false;
        std::string want;
        if (s["type"].is_array()) {
            for (const auto &t : s["type"]) {
                any = any || type_is(v, t.get<std::string>());
                want += (want.empty() ? "" : " or ") + t.get<std::string>();
            }
        } else {
            want = s["type"].get<std::string>();
            any = type_is(v, want);
        }
        if (!any) return name + ": expected " + want + ", got " + v.dump(-1, ' ', false, json::error_handler_t::replace);
    }
    if (s.contains("enum") && s["enum"].is_array()) {
        bool found = false;
        for (const auto &e : s["enum"]) found = found || e == v;
        if (!found) return name + ": must be one of " + s["enum"].dump() + ", got " + v.dump(-1, ' ', false, json::error_handler_t::replace);
    }
    if (v.is_number()) {
        double d = v.get<double>();
        if (s.contains("minimum") && d < s["minimum"].get<double>()) return name + ": must be >= " + s["minimum"].dump();
        if (s.contains("maximum") && d > s["maximum"].get<double>()) return name + ": must be <= " + s["maximum"].dump();
    }
    if (v.is_string()) {
        const std::string &str = v.get_ref<const std::string &>();
        if (s.contains("minLength") && str.size() < s["minLength"].get<size_t>()) return name + ": too short";
        if (s.contains("maxLength") && str.size() > s["maxLength"].get<size_t>()) return name + ": too long (max " + s["maxLength"].dump() + ")";
        if (s.contains("pattern") && s["pattern"].is_string()) {
            try {
                if (!std::regex_search(str, std::regex(s["pattern"].get<std::string>(), std::regex::ECMAScript)))
                    return name + ": does not match " + s["pattern"].get<std::string>();
            } catch (const std::exception &) {
                // A pattern std::regex cannot compile: do not refuse on it.
            }
        }
    }
    if (v.is_array()) {
        if (s.contains("minItems") && v.size() < s["minItems"].get<size_t>()) return name + ": at least " + s["minItems"].dump() + " item(s)";
        if (s.contains("maxItems") && v.size() > s["maxItems"].get<size_t>()) return name + ": at most " + s["maxItems"].dump() + " item(s)";
        if (s.contains("items"))
            for (size_t i = 0; i < v.size(); ++i) {
                std::string e = check_value(s["items"], v[i], name + "[" + std::to_string(i) + "]");
                if (!e.empty()) return e;
            }
    }
    return "";
}

std::string check(const json &schema, const json &args) {
    if (!args.is_object()) return "arguments: expected an object";
    if (!schema.is_object() || !schema.contains("properties") || !schema["properties"].is_object()) return "";
    const json &props = schema["properties"];
    for (const auto &r : schema.value("required", json::array()))
        if (r.is_string() && !args.contains(r.get<std::string>())) return r.get<std::string>() + ": required";
    bool closed = schema.contains("additionalProperties") && schema["additionalProperties"] == false;
    for (auto it = args.begin(); it != args.end(); ++it) {
        if (!props.contains(it.key())) {
            if (closed) return it.key() + ": unknown argument";
            continue;
        }
        std::string e = check_value(props[it.key()], it.value(), it.key());
        if (!e.empty()) return e;
    }
    return "";
}

}  // namespace argcheck

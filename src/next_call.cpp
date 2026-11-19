#include "next_call.h"

namespace next_call {

namespace {

// The next call of an error result, or null.
json for_error(const std::string &code, const std::string &tool, const json &args) {
    if (code == "realm_unavailable") return make("map_load", instance_of(args));  // the server/client realms exist once a map is loaded
    if (code == "main_thread_timeout") return make("status", instance_of(args));
    if (code == "ambiguous_instance") return make("instances_list");
    if (code == "client_crashed" || code == "instance_gone") return make("instances_launch", json{{"count", 1}});
    if (code == "instance_unreachable") return make("status", instance_of(args), 10);  // a game is unreachable for 15-18 s after a start
    if (code == "budget_exceeded" && args.is_object()) {
        // The same code as a job, which runs across frames and has no budget to exceed.
        json a = json::object();
        for (const char *k : {"code", "path", "realm", "instance"})
            if (args.contains(k)) a[k] = args[k];
        if ((tool == "lua_exec" && a.contains("code")) || (tool == "lua_exec_file" && a.contains("path"))) {
            a["async"] = true;
            return make("lua_job", a);
        }
    }
    return nullptr;
}

// The next call of a success result, or null.
json for_success(const json &sc, const std::string &tool, const json &args) {
    if (tool == "lua_job" && sc.value("state", "") == "running" && sc.contains("job_id") && sc["job_id"].is_string()) {
        json a = instance_of(args);
        a["id"] = sc["job_id"];
        a["wait_s"] = 30;  // lua_job_status waits up to this long for the end
        return make("lua_job_status", a);
    }
    if ((tool == "lua_exec" || tool == "lua_exec_file" || tool == "lua_job") && sc.contains("truncated") && sc["truncated"].is_number() &&
        sc["truncated"].get<double>() > 0 && args.is_object()) {
        // Values were cut by the limits the caller passed: the same call without them.
        json a = args;
        bool cut = false;
        for (const char *k : {"depth", "limit", "max_string"}) cut = a.erase(k) > 0 || cut;
        if (cut) return make(tool, a);
    }
    return nullptr;
}

}  // namespace

void attach(json &r, const json &n) {
    if (n.is_null() || !r.is_object() || !r.contains("structuredContent") || !r["structuredContent"].is_object()) return;
    json &sc = r["structuredContent"];
    if (r.value("isError", false)) {
        if (!sc.contains("error") || !sc["error"].is_object()) return;
        sc["error"]["next_call"] = n;
    } else {
        sc["next_call"] = n;
    }
    if (r.contains("content") && r["content"].is_array())
        for (json &c : r["content"])
            if (c.is_object() && c.value("type", "") == "text") {
                c["text"] = sc.dump(-1, ' ', false, json::error_handler_t::replace);
                break;
            }
}

void annotate(json &r, const std::string &tool, const json &args) {
    if (!r.is_object() || !r.contains("structuredContent") || !r["structuredContent"].is_object()) return;
    const json &sc = r["structuredContent"];
    if (r.value("isError", false)) {
        if (!sc.contains("error") || !sc["error"].is_object() || sc["error"].contains("next_call")) return;
        attach(r, for_error(sc["error"].value("code", ""), tool, args));
    } else if (!sc.contains("next_call")) {
        attach(r, for_success(sc, tool, args));
    }
}

}  // namespace next_call

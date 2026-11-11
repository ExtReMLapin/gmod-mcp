#include "mcp_protocol.h"
#include "tasks.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "log.h"
#include "mcp.h"
#include "platform/platform.h"
#include "reqctx.h"
#include "util.h"

namespace tasks {

const char *kExtensionId = "io.modelcontextprotocol/tasks";

using SysClock = std::chrono::system_clock;

static const int64_t kDefaultTtlMs = 3600 * 1000;   // 1 h
static const int64_t kMaxTtlMs = 24 * 3600 * 1000;  // 24 h
static const int kPollIntervalMs = 1000;
static const size_t kMaxActive = 32;

// Internal state: working | done | cancelled. The exposed status depends on the era (isError -> failed in legacy,
// completed with result in modern, where failed is reserved for JSON-RPC errors).
struct Task {
    std::string id;
    std::string tool;
    std::string state = "working";
    std::string status_message;
    SysClock::time_point created, updated;
    int64_t ttl_ms = kDefaultTtlMs;
    json result;  // CallToolResult
    bool is_error = false;
    std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
    std::mutex m;
    std::condition_variable cv;
};

static std::mutex g_mu;
static std::map<std::string, std::shared_ptr<Task>> g_tasks;

static std::string iso(SysClock::time_point tp) {
    std::time_t t = SysClock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

static json rpc_error(int code, const std::string &msg) { return json{{"__rpc_error", {{"code", code}, {"message", msg}}}}; }

static std::string status_of(const Task &t, Era era) {
    if (t.state == "working") return "working";
    if (t.state == "cancelled") return "cancelled";
    return (era == Era::Legacy && t.is_error) ? "failed" : "completed";
}

// Must be called with t.m locked.
static json task_json_locked(Task &t, Era era, bool detailed) {
    json j{{"taskId", t.id}, {"status", status_of(t, era)}, {"createdAt", iso(t.created)}, {"lastUpdatedAt", iso(t.updated)}};
    if (era == Era::Legacy) {
        j["ttl"] = t.ttl_ms;
        j["pollInterval"] = kPollIntervalMs;
    } else {
        j["ttlMs"] = t.ttl_ms;
        j["pollIntervalMs"] = kPollIntervalMs;
    }
    if (!t.status_message.empty()) j["statusMessage"] = t.status_message;
    if (detailed && era == Era::Modern && t.state == "done") j["result"] = t.result;
    return j;
}

static json task_json(Task &t, Era era, bool detailed = false) {
    std::lock_guard<std::mutex> lk(t.m);
    return task_json_locked(t, era, detailed);
}

static void purge_expired_locked() {
    auto now = SysClock::now();
    for (auto it = g_tasks.begin(); it != g_tasks.end();) {
        Task &t = *it->second;
        bool expired;
        {
            std::lock_guard<std::mutex> lk(t.m);
            expired = t.state != "working" && now - t.created > std::chrono::milliseconds(t.ttl_ms);
        }
        it = expired ? g_tasks.erase(it) : std::next(it);
    }
}

json legacy_capability() {
    return json{{"list", json::object()}, {"cancel", json::object()}, {"requests", {{"tools", {{"call", json::object()}}}}}};
}

json create_tool_task(const std::string &tool, const json &args, const json &task_params, Era era) {
    auto t = std::make_shared<Task>();
    unsigned char raw[16];
    if (!plat::random_bytes(raw, sizeof raw)) return rpc_error(mcpproto::err::kInternalError, "failed to generate identifier");
    t->id = util::hex(raw, sizeof raw);  // 128 bits: not guessable
    t->tool = tool;
    t->created = t->updated = SysClock::now();
    if (task_params.is_object()) {
        const char *k = task_params.contains("ttlMs") ? "ttlMs" : "ttl";
        if (task_params.contains(k) && task_params[k].is_number()) {
            int64_t ttl = task_params[k].get<int64_t>();
            t->ttl_ms = ttl <= 0 ? kDefaultTtlMs : (ttl > kMaxTtlMs ? kMaxTtlMs : ttl);
        }
    }
    t->status_message = "Running " + tool;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        purge_expired_locked();
        size_t active = 0;
        for (auto &kv : g_tasks) {
            std::lock_guard<std::mutex> lk2(kv.second->m);
            if (kv.second->state == "working") active++;
        }
        if (active >= kMaxActive) return rpc_error(mcpproto::err::kInternalError, "too many active tasks (max 32)");
        g_tasks[t->id] = t;
    }
    logx::info("task %s created for tool %s (%s)", t->id.c_str(), tool.c_str(), era == Era::Modern ? "extension" : "2025-11-25");
    std::thread([t, tool, args] {
        reqctx::Ctx ctx;
        ctx.cancel = t->cancel;
        ctx.progress = [t](double done, double total, const std::string &msg) {
            std::lock_guard<std::mutex> lk(t->m);
            if (t->state != "working") return;
            std::string m = msg.empty() ? "Running " + t->tool : msg;
            if (total > 0) m += " (" + util::fmt_num(done) + "/" + util::fmt_num(total) + ")";
            t->status_message = m;
            t->updated = SysClock::now();
        };
        json r;
        {
            reqctx::Scope scope(&ctx);
            r = mcp::call_tool(tool, args);
        }
        {
            std::lock_guard<std::mutex> lk(t->m);
            if (t->state == "cancelled") return;  // stays cancelled even if execution succeeded
            t->result = r;
            t->is_error = r.value("isError", false);
            t->state = "done";
            t->status_message = t->is_error ? "The tool returned an error" : "Completed";
            if (t->is_error && r.contains("structuredContent") && r["structuredContent"].contains("error"))
                t->status_message = r["structuredContent"]["error"].value("message", "error");
            t->updated = SysClock::now();
        }
        t->cv.notify_all();
    }).detach();
    if (era == Era::Modern) {
        json res = task_json(*t, era);
        res["resultType"] = "task";
        return res;
    }
    json res{{"task", task_json(*t, era)}};
    res["_meta"] = json{{"io.modelcontextprotocol/model-immediate-response",
                         "Task " + t->id + " started for tool " + tool + "; the result will be available when execution finishes."}};
    return res;
}

static std::shared_ptr<Task> find(const json &params) {
    if (!params.is_object() || !params.contains("taskId") || !params["taskId"].is_string()) return nullptr;
    std::lock_guard<std::mutex> lk(g_mu);
    purge_expired_locked();
    auto it = g_tasks.find(params["taskId"].get<std::string>());
    return it == g_tasks.end() ? nullptr : it->second;
}

json get(const json &params, Era era) {
    auto t = find(params);
    if (!t) return rpc_error(mcpproto::err::kInvalidParams, "Failed to retrieve task: Task not found");
    return task_json(*t, era, true);
}

json result(const json &params) {
    auto t = find(params);
    if (!t) return rpc_error(mcpproto::err::kInvalidParams, "Failed to retrieve task: Task not found");
    std::unique_lock<std::mutex> lk(t->m);
    t->cv.wait(lk, [&] { return t->state != "working"; });
    json r;
    if (t->state == "cancelled") {
        r = json{{"content", json::array({json{{"type", "text"}, {"text", "{\"error\":{\"code\":\"cancelled\",\"message\":\"task cancelled\"}}"}}})},
                 {"isError", true}};
    } else {
        r = t->result;
    }
    r["_meta"] = json{{"io.modelcontextprotocol/related-task", {{"taskId", t->id}}}};
    return r;
}

json list(const json &) {
    std::lock_guard<std::mutex> lk(g_mu);
    purge_expired_locked();
    json arr = json::array();
    for (auto &kv : g_tasks) arr.push_back(task_json(*kv.second, Era::Legacy));
    return json{{"tasks", arr}};
}

json cancel(const json &params, Era era) {
    auto t = find(params);
    if (!t) return rpc_error(mcpproto::err::kInvalidParams, "Failed to retrieve task: Task not found");
    {
        std::lock_guard<std::mutex> lk(t->m);
        if (t->state != "working") {
            if (era == Era::Modern) return json::object();  // cooperative: plain acknowledgement
            return rpc_error(mcpproto::err::kInvalidParams, "Cannot cancel task: already in terminal status '" + status_of(*t, era) + "'");
        }
        t->cancel->store(true);
        t->state = "cancelled";
        t->status_message = "The task was cancelled by request.";
        t->updated = SysClock::now();
    }
    t->cv.notify_all();
    logx::info("task %s cancelled", t->id.c_str());
    if (era == Era::Modern) return json::object();
    return task_json(*t, era);
}

json update(const json &params) {
    auto t = find(params);
    if (!t) return rpc_error(mcpproto::err::kInvalidParams, "Failed to retrieve task: Task not found");
    return json::object();  // our tasks never request input: responses are ignored
}

}  // namespace tasks

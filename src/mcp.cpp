#include "mcp.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>

#include "agent.h"
#include "mcp_protocol.h"
#include "log.h"
#include "prompts.h"
#include "reqctx.h"
#include "resources.h"
#include "tasks.h"
#include "util.h"

namespace mcp {

const char *kProtocolVersion = mcpproto::kLegacy[0];
static const auto &kSupportedVersions = mcpproto::kLegacy;
namespace err = mcpproto::err;

static std::vector<Tool> g_tools;
static std::map<std::string, size_t> g_index;
static RelayFn g_relay;
static PeersFn g_peers;
static std::string g_instructions;

void set_relay(RelayFn fn) { g_relay = std::move(fn); }
void set_peers(PeersFn fn) { g_peers = std::move(fn); }
void set_instructions(std::string s) { g_instructions = std::move(s); }

static const json kInstanceProp = {
    {"type", "string"},
    {"description",
     "Optional. Instance identifier (see instances_list). If present and different from the local instance, the call is "
     "relayed by the coordinator to that instance."},
    // 2026-07-28 Streamable HTTP: the client mirrors the value into the Mcp-Param-Instance header (lets proxies route).
    {"x-mcp-header", "Instance"}};

// Default output schema: every structuredContent is an object naming the instance that answered.
static const json kDefaultOutputSchema = {
    {"type", "object"},
    {"properties", {{"answered_by", {{"type", "string"}, {"description", "Identifier of the game instance that answered."}}}}},
    {"additionalProperties", true}};

void register_tool(Tool t) {
    json &s = t.input_schema;
    if (!s.contains("type")) s["type"] = "object";
    if (!s.contains("properties")) s["properties"] = json::object();
    s["additionalProperties"] = false;
    if (!t.local_only) s["properties"]["instance"] = kInstanceProp;
    if (t.annotations.is_null()) t.annotations = json::object();
    t.annotations["title"] = t.title;
    if (!t.annotations.contains("openWorldHint")) t.annotations["openWorldHint"] = false;
    if (t.output_schema.is_null()) t.output_schema = kDefaultOutputSchema;
    t.validator = std::make_shared<nlohmann::json_schema::json_validator>();
    t.validator->set_root_schema(s);
    g_index[t.name] = g_tools.size();
    g_tools.push_back(std::move(t));
}

const std::vector<Tool> &tools() { return g_tools; }

void set_task_support(const std::string &name, const std::string &mode) {
    auto it = g_index.find(name);
    if (it != g_index.end()) g_tools[it->second].task_support = mode;
}

static json unwrap_rpc(const json &id, const json &r);
static json to_call_result(json r);

const Tool *find_tool(const std::string &name) {
    auto it = g_index.find(name);
    return it == g_index.end() ? nullptr : &g_tools[it->second];
}

// --- Validation (json-schema-validator, validator compiled once per tool) ---
std::string validate(const json &schema, const json &value, const std::string &) {
    try {
        nlohmann::json_schema::json_validator v;
        v.set_root_schema(schema);
        v.validate(value);
    } catch (const std::exception &e) {
        return e.what();
    }
    return "";
}

static size_t edit_distance(const std::string &a, const std::string &b) {
    std::vector<size_t> row(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) row[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        size_t diag = row[0];
        row[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            size_t up = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diag + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diag = up;
        }
    }
    return row[b.size()];
}

// The validator's message alone ("unexpected instance additional property") rarely tells an agent what to send: name
// the unknown and missing arguments, suggest the closest accepted name, and list what the tool accepts.
static std::string argument_hint(const Tool &t, const json &value) {
    const json &props = t.input_schema["properties"];
    std::string out;
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (props.contains(it.key())) continue;
            std::string best;
            size_t best_d = 4;
            for (auto p = props.begin(); p != props.end(); ++p) {
                const std::string &k = p.key(), &u = it.key();
                size_t d = edit_distance(u, k);
                if (k.find(u) != std::string::npos || u.find(k) != std::string::npos) d = std::min<size_t>(d, 1);
                if (d < best_d) best_d = d, best = k;
            }
            out += " Unknown argument '" + it.key() + "'" + (best.empty() ? "." : " (did you mean '" + best + "'?).");
        }
    }
    json req = t.input_schema.value("required", json::array());
    for (const auto &r : req)
        if (!value.is_object() || !value.contains(r.get<std::string>())) out += " Missing required argument '" + r.get<std::string>() + "'.";
    std::string list;
    for (auto p = props.begin(); p != props.end(); ++p) {
        if (!list.empty()) list += ", ";
        list += p.key();
        if (p.value().contains("type") && p.value()["type"].is_string()) list += " (" + p.value()["type"].get<std::string>() + ")";
    }
    out += " Accepted arguments: " + list + ".";
    return out;
}

static std::string validate_tool(const Tool &t, const json &value) {
    try {
        t.validator->validate(value);
    } catch (const std::exception &e) {
        return std::string("invalid arguments: ") + e.what() + "." + argument_hint(t, value);
    }
    return "";
}

// --- Response building ---
static json rpc_result(const json &id, json result) { return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}; }

static json rpc_error(const json &id, int code, const std::string &msg, json data = nullptr) {
    json e{{"code", code}, {"message", msg}};
    if (!data.is_null()) e["data"] = data;
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", e}};
}

static json to_call_result(json r) {
    json content = json::array();
    if (!r.is_object()) r = json{{"ok", false}, {"error", {{"code", "internal"}, {"message", "invalid tool result"}}}};
    bool ok = r.value("ok", false);
    if (!ok) {
        json e = r.value("error", json{{"code", "internal"}, {"message", "unknown error"}});
        json sc{{"error", e}, {"answered_by", agent::config().instance_id}};
        content.push_back(json{{"type", "text"}, {"text", sc.dump(-1, ' ', false, json::error_handler_t::replace)}});
        return json{{"content", content}, {"structuredContent", sc}, {"isError", true}};
    }
    json result = r.value("result", json::object());
    if (!result.is_object()) result = json{{"value", result}};
    // Always say which game process answered (several instances may share the endpoint).
    result["answered_by"] = agent::config().instance_id;
    if (r.contains("image")) {
        content.push_back(json{{"type", "image"}, {"data", r["image"]["data"]}, {"mimeType", r["image"]["mimeType"]}});
    }
    content.insert(content.begin(), json{{"type", "text"}, {"text", result.dump(-1, ' ', false, json::error_handler_t::replace)}});
    return json{{"content", content}, {"structuredContent", result}, {"isError", false}};
}

json call_tool(const std::string &name, const json &args_in) {
    const Tool *t = find_tool(name);
    if (!t) return to_call_result(agent::err("unknown_tool", "unknown tool: " + name));
    json args = args_in.is_null() ? json::object() : args_in;
    std::string verr = validate_tool(*t, args);
    if (!verr.empty()) return to_call_result(agent::err("invalid_params", verr));
    if (args.contains("instance") && !t->local_only) {
        std::string inst = args["instance"].get<std::string>();
        args.erase("instance");
        if (inst != agent::config().instance_id) {
            if (!g_relay) return to_call_result(agent::err("not_coordinator", "relay unavailable: this instance is not the coordinator"));
            return g_relay(inst, name, args);  // already a CallToolResult
        }
    } else if (!t->local_only && g_peers) {
        // Several game processes share this endpoint: a call without `instance` would silently target whichever
        // instance holds the base port (it changes after a coordinator re-election). Refuse instead of guessing.
        static const char *exempt[] = {"status", "wiki_search", "wiki_page"};
        bool is_exempt = false;
        for (const char *e : exempt) is_exempt = is_exempt || name == e;
        std::vector<std::string> peers = g_peers();
        if (!is_exempt && !peers.empty()) {
            json ids = json::array({agent::config().instance_id + " (this one)"});
            for (const auto &p : peers) ids.push_back(p);
            return to_call_result(agent::err("ambiguous_instance",
                                             "several game instances share this endpoint: pass `instance` (see instances_list)",
                                             json{{"instances", ids}}));
        }
    }
    json r;
    try {
        r = t->handler(args);
    } catch (const std::exception &e) {
        r = agent::err("internal", std::string("exception: ") + e.what());
    }
    return to_call_result(std::move(r));
}

// Small generic icon (SVG): a game controller on a blue rounded square.
static json icons() {
    static const std::string svg =
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 64 64'><rect width='64' height='64' rx='14' fill='#1565c0'/>"
        "<path d='M18 24h28a10 10 0 0 1 10 10v2a8 8 0 0 1-14 5l-3-3H25l-3 3a8 8 0 0 1-14-5v-2a10 10 0 0 1 10-10z' fill='#fff'/>"
        "<path d='M20 30h4v4h4v4h-4v4h-4v-4h-4v-4h4z' fill='#1565c0' transform='translate(0 -4)'/>"
        "<circle cx='42' cy='31' r='2.5' fill='#1565c0'/><circle cx='47' cy='35' r='2.5' fill='#1565c0'/></svg>";
    static const std::string uri = "data:image/svg+xml;base64," + util::base64_encode(svg);
    return json::array({json{{"src", uri}, {"mimeType", "image/svg+xml"}, {"sizes", json::array({"any"})}}});
}

static json server_info() {
    return json{{"name", "gmod-mcp"},
                {"title", "Garry's Mod MCP agent"},
                {"version", agent::config().version},
                {"description", "Control of a local Garry's Mod instance: Lua in the menu/client/server realms, maps, "
                                "console, entities, rendering, sound, performance and documentation."},
                {"icons", icons()}};
}

static const char *kModernVersion = mcpproto::kModern;
static const char *kSubscriptionMeta = mcpproto::kMetaSubscriptionId;

// Request context: protocol era and capabilities declared by the client (modern era: per request).
struct Ctx {
    tasks::Era era = tasks::Era::Legacy;
    json client_caps = json::object();
    int http_status = 200;
    const Emit *emit = nullptr;  // SSE response stream, if any
};

struct Prepared {
    json msg;
    json id;
    std::string method;
    json params;
    Ctx ctx;
    bool notification = false;
    bool stream_candidate = false;
};

static bool client_supports_tasks(const Ctx &c) {
    return c.era == tasks::Era::Modern && c.client_caps.is_object() && c.client_caps.contains("extensions") &&
           c.client_caps["extensions"].is_object() && c.client_caps["extensions"].contains(tasks::kExtensionId);
}

static bool client_supports_elicitation(const Ctx &c) {
    return c.era == tasks::Era::Modern && c.client_caps.is_object() && c.client_caps.contains("elicitation");
}

// Server-side decision (Tasks extension): always for map_load; other "optional" tools only for an explicit wait of at
// least 15 s.
static bool server_wants_task(const Tool &t, const json &args) {
    if (t.task_support.empty() || t.task_support == "forbidden") return false;
    if (t.name == "map_load") return true;
    for (const char *k : {"timeout_s", "duration_s", "wait_s"})
        if (args.contains(k) && args[k].is_number() && args[k].get<double>() >= 15.0) return true;
    return false;
}

static json tools_list_json(const Ctx &c) {
    json arr = json::array();
    for (const auto &t : g_tools) {
        json j{{"name", t.name}, {"title", t.title}, {"description", t.description}, {"inputSchema", t.input_schema},
               {"outputSchema", t.output_schema}, {"annotations", t.annotations}};
        // execution.taskSupport only exists in the experimental 2025-11-25 Tasks (with the extension, the server decides).
        if (c.era == tasks::Era::Legacy && !t.task_support.empty()) j["execution"] = json{{"taskSupport", t.task_support}};
        arr.push_back(std::move(j));
    }
    return arr;
}

static json server_capabilities(const Ctx &c) {
    // Resource subscriptions only exist through subscriptions/listen (2026-07-28); the legacy resources/subscribe would
    // need a standalone GET stream, which this server does not offer.
    json caps{{"tools", {{"listChanged", false}}},
              {"resources", {{"listChanged", false}, {"subscribe", c.era == tasks::Era::Modern}}},
              {"prompts", {{"listChanged", false}}},
              {"completions", json::object()}};
    if (c.era == tasks::Era::Legacy) caps["tasks"] = tasks::legacy_capability();
    else caps["extensions"] = json{{tasks::kExtensionId, json::object()}};
    return caps;
}

// Lists are returned in a single page: any cursor is one this server never issued.
static bool cursor_ok(const json &params) { return !params.contains("cursor") || params["cursor"].is_null(); }

static json notification(const std::string &method, json params) {
    return json{{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}};
}

static const size_t kMaxSubscriptions = 8;
static std::atomic<size_t> g_subscriptions{0};

// subscriptions/listen: acknowledges the honored filter, then notifies resource updates until the client closes the
// stream (the transport sets the cancel flag).
static json listen(const json &id, const json &params, Ctx &c) {
    if (!c.emit) return rpc_error(id, err::kInvalidRequest, "subscriptions/listen needs an SSE response: send Accept: text/event-stream");
    if (g_subscriptions.load() >= kMaxSubscriptions) return rpc_error(id, err::kInternalError, "too many open subscriptions (max 8)");
    struct Count {
        Count() { ++g_subscriptions; }
        ~Count() { --g_subscriptions; }
    } count;
    json filter = params.value("notifications", json::object());
    json honored = json::object();
    std::vector<std::string> uris;
    if (filter.is_object() && filter.contains("resourceSubscriptions") && filter["resourceSubscriptions"].is_array()) {
        for (const auto &u : filter["resourceSubscriptions"])
            if (u.is_string() && resources::subscribable(u)) uris.push_back(u);
        honored["resourceSubscriptions"] = uris;
    }
    json meta{{kSubscriptionMeta, id}};
    (*c.emit)(notification("notifications/subscriptions/acknowledged", json{{"notifications", honored}, {"_meta", meta}}));
    std::map<std::string, uint64_t> seen;
    std::map<std::string, std::chrono::steady_clock::time_point> last_sent;
    for (const auto &u : uris) seen[u] = resources::version(u);
    while (!reqctx::cancelled()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        auto now = std::chrono::steady_clock::now();
        for (const auto &u : uris) {
            uint64_t v = resources::version(u);
            // At most one update per resource and per second (the console can print hundreds of lines per second).
            if (v == seen[u] || now - last_sent[u] < std::chrono::seconds(1)) continue;
            seen[u] = v;
            last_sent[u] = now;
            (*c.emit)(notification("notifications/resources/updated", json{{"uri", u}, {"_meta", meta}}));
        }
    }
    return rpc_result(id, json{{"_meta", meta}});
}

// MRTR (2026-07-28): a tool error carrying details.confirm {arg, message} becomes an elicitation request when the
// client supports it; the client retries with inputResponses.confirm and the server sets args[arg] = true on "accept".
static json confirm_request(const json &confirm) {
    json schema{{"type", "object"},
                {"properties", {{"confirm", {{"type", "boolean"}, {"title", "Allow"}, {"default", false}}}}},
                {"required", json::array({"confirm"})}};
    json req{{"method", "elicitation/create"},
             {"params", {{"mode", "form"}, {"message", confirm.value("message", "Allow this action?")}, {"requestedSchema", schema}}}};
    return json{{"resultType", "input_required"},
                {"inputRequests", {{"confirm", req}}},
                {"requestState", "confirm:" + confirm.value("arg", "")}};
}

static json call_with_confirmation(const json &id, const std::string &name, json a, const json &params, const Ctx &c) {
    bool answered = false;
    if (params.contains("inputResponses") && params["inputResponses"].is_object() && params["inputResponses"].contains("confirm")) {
        answered = true;
        const json &resp = params["inputResponses"]["confirm"];
        std::string state = params.value("requestState", "");
        bool accepted = resp.is_object() && resp.value("action", "") == "accept" && resp.contains("content") &&
                        resp["content"].is_object() && resp["content"].value("confirm", false) == true;
        if (accepted && state.rfind("confirm:", 0) == 0 && state.size() > 8) a[state.substr(8)] = true;
    }
    json r = call_tool(name, a);
    if (!answered && client_supports_elicitation(c) && r.value("isError", false) && r.contains("structuredContent")) {
        const json &e = r["structuredContent"].value("error", json::object());
        if (e.contains("details") && e["details"].is_object() && e["details"].contains("confirm") && e["details"]["confirm"].is_object())
            return rpc_result(id, confirm_request(e["details"]["confirm"]));
    }
    return rpc_result(id, r);
}

static json complete(const json &id, const json &params) {
    json ref = params.value("ref", json::object());
    json argument = params.value("argument", json::object());
    if (!ref.is_object() || !argument.is_object() || !argument.contains("name") || !argument["name"].is_string())
        return rpc_error(id, err::kInvalidParams, "completion/complete: expected ref and argument {name, value}");
    std::string argname = argument["name"], value = argument.value("value", "");
    std::string type = ref.value("type", "");
    std::vector<std::string> values;
    if (type == "ref/prompt") values = prompts::complete(ref.value("name", ""), argname, value);
    else if (type == "ref/resource") values = resources::complete(ref.value("uri", ""), argname, value);
    else return rpc_error(id, err::kInvalidParams, "completion/complete: unknown ref type '" + type + "'");
    size_t total = values.size();
    if (values.size() > 100) values.resize(100);
    return rpc_result(id, json{{"completion", {{"values", values}, {"total", total}, {"hasMore", total > values.size()}}}});
}

static json handle_message(const json &msg, bool &is_notification, Ctx &c) {
    is_notification = !msg.contains("id");
    json id = msg.value("id", json(nullptr));
    if (!msg.is_object() || msg.value("jsonrpc", "") != "2.0" || !msg.contains("method") || !msg["method"].is_string())
        return rpc_error(id, err::kInvalidRequest, "invalid JSON-RPC request");
    std::string method = msg["method"];
    json params = msg.value("params", json::object());
    if (!params.is_object()) params = json::object();

    if (is_notification) {
        // Legacy cancellation of an in-flight request (modern clients close the response stream instead).
        if (method == "notifications/cancelled" && params.contains("requestId")) {
            if (reqctx::cancel_tracked(params["requestId"].dump())) logx::info("request %s cancelled by the client", params["requestId"].dump().c_str());
        }
        return nullptr;
    }

    // Removed in 2026-07-28: answered as unknown methods (HTTP 404, err::kMethodNotFound).
    if (c.era == tasks::Era::Modern && (method == "initialize" || method == "ping")) {
        c.http_status = 404;
        return rpc_error(id, err::kMethodNotFound, std::string("method not found in protocol version ") + mcpproto::kModern + ": " + method);
    }
    if (method == "initialize") {
        std::string asked = params.value("protocolVersion", "");
        logx::info("initialize: client=%s protocol=%s capabilities=%s", params.value("clientInfo", json::object()).dump().c_str(),
                   asked.c_str(), params.value("capabilities", json::object()).dump().c_str());
        std::string ver = kProtocolVersion;
        for (const char *v : kSupportedVersions)
            if (asked == v) ver = v;
        return rpc_result(id, json{{"protocolVersion", ver},
                                   {"capabilities", server_capabilities(c)},
                                   {"serverInfo", server_info()},
                                   {"instructions", g_instructions}});
    }
    if (method == "server/discover") {
        json meta = params.value("_meta", json::object());
        logx::info("server/discover: client=%s capabilities=%s",
                   meta.value(mcpproto::kMetaClientInfo, json::object()).dump().c_str(),
                   meta.value(mcpproto::kMetaClientCapabilities, json::object()).dump().c_str());
        Ctx modern = c;
        modern.era = tasks::Era::Modern;
        json versions = json::array({kModernVersion});
        for (const char *v : kSupportedVersions) versions.push_back(v);
        return rpc_result(id, json{{"supportedVersions", versions},
                                   {"capabilities", server_capabilities(modern)},
                                   {"instructions", g_instructions},
                                   {"ttlMs", 0},
                                   {"cacheScope", "private"}});
    }
    if (method == "ping") return rpc_result(id, json::object());
    bool is_list = method == "tools/list" || method == "prompts/list" || method == "resources/list" || method == "resources/templates/list";
    if (is_list && !cursor_ok(params)) return rpc_error(id, err::kInvalidParams, "invalid cursor (this server returns every list in one page)");
    if (method == "tools/list") return rpc_result(id, json{{"tools", tools_list_json(c)}, {"ttlMs", 0}, {"cacheScope", "private"}});
    if (method == "tools/call") {
        if (!params.contains("name") || !params["name"].is_string()) return rpc_error(id, err::kInvalidParams, "missing params.name");
        std::string name = params["name"];
        const Tool *t = find_tool(name);
        if (!t) return rpc_error(id, err::kInvalidParams, "unknown tool: " + name);
        json a = params.value("arguments", json::object());
        if (a.is_null()) a = json::object();
        bool legacy_task = c.era == tasks::Era::Legacy && params.contains("task") && params["task"].is_object();
        bool modern_task = client_supports_tasks(c) && server_wants_task(*t, a);
        if (legacy_task && (t->task_support.empty() || t->task_support == "forbidden"))
            return rpc_error(id, err::kMethodNotFound, "tool " + name + " does not support task execution (execution.taskSupport absent)");
        if (legacy_task || modern_task) {
            std::string verr = validate_tool(*t, a);
            if (!verr.empty()) return rpc_result(id, to_call_result(agent::err("invalid_params", verr)));
            return unwrap_rpc(id, tasks::create_tool_task(name, a, params.value("task", json::object()), c.era));
        }
        if (c.era == tasks::Era::Legacy && t->task_support == "required")
            return rpc_error(id, err::kInvalidRequest, "Task augmentation required for tools/call requests");
        return call_with_confirmation(id, name, a, params, c);
    }
    // 2026-07-28: the tasks methods belong to the Tasks extension; a client that did not declare it gets
    // MissingRequiredClientCapability (-32021, HTTP 400) naming the capability.
    if (c.era == tasks::Era::Modern && !client_supports_tasks(c) &&
        (method == "tasks/get" || method == "tasks/cancel" || method == "tasks/update")) {
        c.http_status = 400;
        return rpc_error(id, err::kMissingRequiredClientCapability,
                         method + " needs the client capability extensions." + tasks::kExtensionId,
                         json{{"requiredCapabilities", {{"extensions", {{tasks::kExtensionId, json::object()}}}}}});
    }
    if (method == "tasks/get") return unwrap_rpc(id, tasks::get(params, c.era));
    if (method == "tasks/cancel") return unwrap_rpc(id, tasks::cancel(params, c.era));
    if (c.era == tasks::Era::Legacy && method == "tasks/result") return unwrap_rpc(id, tasks::result(params));
    if (c.era == tasks::Era::Legacy && method == "tasks/list") return unwrap_rpc(id, tasks::list(params));
    if (c.era == tasks::Era::Modern && method == "tasks/update") return unwrap_rpc(id, tasks::update(params));
    if (method == "resources/list") return rpc_result(id, json{{"resources", resources::list()}, {"ttlMs", 0}, {"cacheScope", "private"}});
    if (method == "resources/templates/list")
        return rpc_result(id, json{{"resourceTemplates", resources::templates()}, {"ttlMs", 0}, {"cacheScope", "private"}});
    if (method == "resources/read") {
        std::string uri = params.value("uri", "");
        json res = resources::read(uri);
        // Not found: err::kInvalidParams since 2026-07-28 (err::kResourceNotFoundLegacy before).
        if (res.is_null()) return rpc_error(id, c.era == tasks::Era::Modern ? err::kInvalidParams : err::kResourceNotFoundLegacy, "resource not found", json{{"uri", uri}});
        if (res.contains("__error"))
            return rpc_error(id, err::kInternalError, res["__error"].value("message", "resource unavailable"), json{{"uri", uri}, {"code", res["__error"]["code"]}});
        return rpc_result(id, json{{"contents", json::array({res})}, {"ttlMs", 0}, {"cacheScope", "private"}});
    }
    if (method == "prompts/list") return rpc_result(id, json{{"prompts", prompts::list()}, {"ttlMs", 0}, {"cacheScope", "private"}});
    if (method == "prompts/get") {
        std::string err;
        json r = prompts::get(params.value("name", ""), params.value("arguments", json::object()), err);
        if (r.is_null()) return rpc_error(id, err::kInvalidParams, err);
        return rpc_result(id, r);
    }
    if (method == "completion/complete") return complete(id, params);
    if (c.era == tasks::Era::Modern && method == "subscriptions/listen") return listen(id, params, c);
    if (c.era == tasks::Era::Modern) c.http_status = 404;  // modern era: unknown method = 404 + -32601
    return rpc_error(id, err::kMethodNotFound, "unknown method: " + method);
}

static json unwrap_rpc(const json &id, const json &r) {
    if (r.is_object() && r.contains("__rpc_error"))
        return rpc_error(id, r["__rpc_error"]["code"].get<int>(), r["__rpc_error"]["message"].get<std::string>());
    return rpc_result(id, r);
}

static std::string header(const Headers &h, const char *name) {
    auto it = h.find(name);
    return it == h.end() ? "" : it->second;
}

// Strict Base64 (standard alphabet, padded): what the "=?base64?...?=" sentinel must carry.
static bool base64_strict(const std::string &b) {
    if (b.size() % 4 != 0) return false;
    size_t pad = 0;
    for (size_t i = 0; i < b.size(); ++i) {
        char c = b[i];
        if (c == '=') {
            if (i + 2 < b.size()) return false;  // padding only in the last two positions
            ++pad;
            continue;
        }
        if (pad) return false;  // data after padding
        if (!(std::isalnum((unsigned char)c) || c == '+' || c == '/')) return false;
    }
    return pad <= 2;
}

// Decodes an Mcp-Name / Mcp-Param-* header value (plain, or the "=?base64?...?=" sentinel). Returns false when the
// value is malformed: invalid Base64 in the sentinel, or a plain value with characters a client must encode.
static bool decode_header_value(const std::string &v, std::string &out) {
    const std::string pre = "=?base64?", suf = "?=";
    if (v.size() >= pre.size() + suf.size() && v.compare(0, pre.size(), pre) == 0 &&
        v.compare(v.size() - suf.size(), suf.size(), suf) == 0) {
        std::string b = v.substr(pre.size(), v.size() - pre.size() - suf.size());
        if (!base64_strict(b)) return false;
        out = util::base64_decode(b);
        return true;
    }
    for (unsigned char c : v)
        if (c < 0x20 || c > 0x7e) return false;
    if (!v.empty() && (v.front() == ' ' || v.back() == ' ')) return false;
    out = v;
    return true;
}

// Maximum JSON nesting depth, checked before parsing: the recursive parser would otherwise overflow the network
// thread stack on hostile input such as 100k nested arrays.
static bool nesting_ok(const std::string &s, int max_depth) {
    int depth = 0;
    bool in_str = false, esc = false;
    for (char c : s) {
        if (in_str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') in_str = true;
        else if (c == '[' || c == '{') {
            if (++depth > max_depth) return false;
        } else if (c == ']' || c == '}') {
            --depth;
        }
    }
    return true;
}

static std::string dump(const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

static std::shared_ptr<Prepared> prepare_inner(const std::string &body, const Headers &headers, std::string &immediate, int &http_status) {
    auto fail = [&](int status, const json &id, int code, const std::string &msg, json data = nullptr) -> std::shared_ptr<Prepared> {
        http_status = status;
        immediate = dump(rpc_error(id, code, msg, std::move(data)));
        return nullptr;
    };
    if (!nesting_ok(body, 64)) return fail(400, nullptr, err::kInvalidRequest, "JSON nesting too deep (max 64)");
    auto p = std::make_shared<Prepared>();
    try {
        p->msg = json::parse(body);
    } catch (const std::exception &) {
        return fail(400, nullptr, err::kParseError, "invalid JSON");
    }
    if (p->msg.is_array()) return fail(400, nullptr, err::kInvalidRequest, "JSON-RPC batching is not supported");
    if (!p->msg.is_object()) return fail(400, nullptr, err::kInvalidRequest, "a JSON-RPC message must be an object");
    json &msg = p->msg;
    p->id = msg.value("id", json(nullptr));
    p->notification = !msg.contains("id");
    p->method = msg.contains("method") && msg["method"].is_string() ? msg["method"].get<std::string>() : "";
    p->params = msg.contains("params") && msg["params"].is_object() ? msg["params"] : json::object();
    const json &id = p->id;
    const json &params = p->params;
    Ctx &c = p->ctx;
    std::string hv = header(headers, "mcp-protocol-version");
    json meta = params.value("_meta", json::object());
    std::string mv = meta.is_object() ? meta.value(mcpproto::kMetaProtocolVersion, "") : "";
    bool is_modern = hv == kModernVersion || !mv.empty();
    if (is_modern) {
        // 2026-07-28 era. A header contradicting _meta is a HeaderMismatch, whatever the versions are.
        if (!mv.empty() && !hv.empty() && hv != mv)
            return fail(400, id, err::kHeaderMismatch, "Header mismatch: MCP-Protocol-Version '" + hv + "' differs from _meta '" + mv + "'");
        // The version must be supported.
        std::string ver = !mv.empty() ? mv : hv;
        if (ver != kModernVersion) {
            bool legacy_known = false;
            for (const char *v : kSupportedVersions) legacy_known = legacy_known || ver == v;
            if (!legacy_known) {
                json sup = json::array({kModernVersion});
                for (const char *v : kSupportedVersions) sup.push_back(v);
                return fail(400, id, err::kUnsupportedProtocolVersion, "Unsupported protocol version: " + ver, json{{"supported", sup}, {"requested", ver}});
            }
            is_modern = false;
        }
    }
    if (is_modern) {
        const std::string &method = p->method;
        auto mismatch = [&](const std::string &why) { return fail(400, id, err::kHeaderMismatch, "Header mismatch: " + why); };
        if (hv.empty()) return mismatch("missing MCP-Protocol-Version header");
        if (!mv.empty() && hv != mv) return mismatch("MCP-Protocol-Version '" + hv + "' differs from _meta '" + mv + "'");  // (also checked above)
        std::string hm = header(headers, "mcp-method");
        if (hm.empty()) return mismatch("missing Mcp-Method header");
        if (hm != method) return mismatch("Mcp-Method '" + hm + "' differs from method '" + method + "'");
        if (method == "tools/call" || method == "resources/read" || method == "prompts/get") {
            std::string body_name = params.value(method == "resources/read" ? "uri" : "name", "");
            std::string hn;
            if (!headers.count("mcp-name")) return mismatch("missing Mcp-Name header");
            if (!decode_header_value(header(headers, "mcp-name"), hn)) return mismatch("malformed Mcp-Name header value");
            if (hn != body_name) return mismatch("Mcp-Name '" + hn + "' differs from '" + body_name + "'");
        }
        // x-mcp-header on `instance`: a value in the body requires the header, and both must agree (null or absent:
        // no header expected).
        if (method == "tools/call") {
            json a = params.value("arguments", json::object());
            bool in_body = a.is_object() && a.contains("instance") && !a["instance"].is_null();
            bool in_header = headers.count("mcp-param-instance") > 0;
            if (in_body && !in_header) return mismatch("missing Mcp-Param-Instance header (arguments.instance is set)");
            if (in_header) {
                std::string hi;
                if (!decode_header_value(header(headers, "mcp-param-instance"), hi)) return mismatch("malformed Mcp-Param-Instance header value");
                std::string bi = in_body && a["instance"].is_string() ? a["instance"].get<std::string>() : "";
                if (!in_body || hi != bi) return mismatch("Mcp-Param-Instance '" + hi + "' differs from arguments.instance '" + bi + "'");
            }
        }
        // Required per-request fields (2026-07-28 base protocol): err::kInvalidParams and HTTP 400 when missing.
        if (!p->notification) {
            if (mv.empty()) return fail(400, id, err::kInvalidParams, std::string("missing _meta ") + mcpproto::kMetaProtocolVersion);
            if (!meta.contains(mcpproto::kMetaClientCapabilities) || !meta[mcpproto::kMetaClientCapabilities].is_object())
                return fail(400, id, err::kInvalidParams, std::string("missing _meta ") + mcpproto::kMetaClientCapabilities);
        }
        c.era = tasks::Era::Modern;
        c.client_caps = meta.value(mcpproto::kMetaClientCapabilities, json::object());
    }
    // Stream candidates: subscriptions, and tool calls that run in place (not as a task) and either asked for progress
    // or may run for a while (cancellation = closing the stream).
    if (!p->notification && c.era == tasks::Era::Modern && p->method == "subscriptions/listen") p->stream_candidate = true;
    if (!p->notification && p->method == "tools/call" && params.contains("name") && params["name"].is_string()) {
        const Tool *t = find_tool(params["name"]);
        json a = params.value("arguments", json::object());
        if (!a.is_object()) a = json::object();
        bool task = (c.era == tasks::Era::Legacy && params.contains("task")) || (t && client_supports_tasks(c) && server_wants_task(*t, a));
        bool progress = meta.is_object() && meta.contains("progressToken");
        if (t && !task && (progress || !t->task_support.empty())) p->stream_candidate = true;
    }
    return p;
}

std::shared_ptr<Prepared> prepare(const std::string &body, const Headers &headers, std::string &immediate, int &http_status) {
    try {
        return prepare_inner(body, headers, immediate, http_status);
    } catch (const std::exception &e) {
        http_status = 500;
        immediate = dump(rpc_error(nullptr, err::kInternalError, std::string("internal error: ") + e.what()));
        return nullptr;
    }
}

bool wants_stream(const Prepared &p) { return p.stream_candidate; }

static std::string execute_inner(Prepared &p, int &http_status, const Emit &emit, std::shared_ptr<std::atomic<bool>> cancel) {
    Ctx &c = p.ctx;
    c.emit = emit ? &emit : nullptr;
    reqctx::Ctx rc;
    if (cancel) rc.cancel = cancel;
    // Progress notifications (on the response stream) when the client sent a progressToken.
    json meta = p.params.value("_meta", json::object());
    if (emit && meta.is_object() && meta.contains("progressToken")) {
        json token = meta["progressToken"];
        auto last = std::make_shared<double>(-1.0);
        auto mu = std::make_shared<std::mutex>();
        rc.progress = [token, last, mu, &emit](double done, double total, const std::string &message) {
            std::lock_guard<std::mutex> lk(*mu);
            if (done <= *last) done = *last + 0.001;  // progress must increase with every notification
            *last = done;
            json np{{"progressToken", token}, {"progress", done}};
            if (total > 0) np["total"] = total;
            if (!message.empty()) np["message"] = message;
            emit(notification("notifications/progress", np));
        };
    }
    std::string track_key = p.notification ? "" : p.id.dump();
    if (!track_key.empty()) reqctx::track(track_key, rc.cancel);
    bool notif = false;
    json r;
    {
        reqctx::Scope scope(&rc);
        r = handle_message(p.msg, notif, c);
    }
    if (!track_key.empty()) reqctx::untrack(track_key);
    if (notif) {
        http_status = 202;
        return "";
    }
    if (c.era == tasks::Era::Modern && r.contains("result") && r["result"].is_object()) {
        json &res = r["result"];
        if (!res.contains("resultType")) res["resultType"] = "complete";
        // 2026-07-28: servers SHOULD identify themselves in every result.
        if (!res.contains("_meta") || !res["_meta"].is_object()) res["_meta"] = json::object();
        res["_meta"][mcpproto::kMetaServerInfo] = server_info();
    }
    http_status = c.http_status;
    return dump(r);
}

std::string execute(Prepared &p, int &http_status, const Emit &emit, std::shared_ptr<std::atomic<bool>> cancel) {
    try {
        return execute_inner(p, http_status, emit, std::move(cancel));
    } catch (const std::exception &e) {
        http_status = 500;
        return dump(rpc_error(p.id, err::kInternalError, std::string("internal error: ") + e.what()));
    }
}

std::string handle(const std::string &body, const Headers &headers, int &http_status) {
    std::string immediate;
    auto p = prepare(body, headers, immediate, http_status);
    if (!p) return immediate;
    return execute(*p, http_status, nullptr, nullptr);
}

}  // namespace mcp

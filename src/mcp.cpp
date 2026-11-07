#include "mcp.h"

#include <map>
#include <mutex>

#include "agent.h"
#include "log.h"
#include "resources.h"
#include "tasks.h"

namespace mcp {

const char *kProtocolVersion = "2025-11-25";
static const char *kSupportedVersions[] = {"2025-11-25", "2025-06-18", "2025-03-26"};

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
     "relayed by the coordinator to that instance."}};

void register_tool(Tool t) {
    json &s = t.input_schema;
    if (!s.contains("type")) s["type"] = "object";
    if (!s.contains("properties")) s["properties"] = json::object();
    s["additionalProperties"] = false;
    if (!t.local_only) s["properties"]["instance"] = kInstanceProp;
    if (t.annotations.is_null()) t.annotations = json::object();
    t.annotations["title"] = t.title;
    if (!t.annotations.contains("openWorldHint")) t.annotations["openWorldHint"] = false;
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

static std::string validate_tool(const Tool &t, const json &value) {
    try {
        t.validator->validate(value);
    } catch (const std::exception &e) {
        return std::string("invalid arguments: ") + e.what();
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

static json server_info() {
    return json{{"name", "gmod-mcp"},
                {"title", "Garry's Mod MCP agent"},
                {"version", agent::config().version},
                {"description", "Control of a local Garry's Mod instance: Lua in the menu/client/server realms, maps, "
                                "console, entities, rendering, sound, performance and documentation."}};
}

static const char *kModernVersion = "2026-07-28";

// Request context: protocol era and capabilities declared by the client (modern era: per request).
struct Ctx {
    tasks::Era era = tasks::Era::Legacy;
    json client_caps = json::object();
    int http_status = 200;
};

static bool client_supports_tasks(const Ctx &c) {
    return c.era == tasks::Era::Modern && c.client_caps.is_object() && c.client_caps.contains("extensions") &&
           c.client_caps["extensions"].is_object() && c.client_caps["extensions"].contains(tasks::kExtensionId);
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
               {"annotations", t.annotations}};
        // execution.taskSupport only exists in the experimental 2025-11-25 Tasks (with the extension, the server decides).
        if (c.era == tasks::Era::Legacy && !t.task_support.empty()) j["execution"] = json{{"taskSupport", t.task_support}};
        arr.push_back(std::move(j));
    }
    return arr;
}

static json server_capabilities(const Ctx &c) {
    json caps{{"tools", {{"listChanged", false}}}, {"resources", {{"listChanged", false}}}};
    if (c.era == tasks::Era::Legacy) caps["tasks"] = tasks::legacy_capability();
    else caps["extensions"] = json{{tasks::kExtensionId, json::object()}};
    return caps;
}

static json handle_message(const json &msg, bool &is_notification, Ctx &c) {
    is_notification = !msg.contains("id");
    json id = msg.value("id", json(nullptr));
    if (!msg.is_object() || msg.value("jsonrpc", "") != "2.0" || !msg.contains("method") || !msg["method"].is_string())
        return rpc_error(id, -32600, "invalid JSON-RPC request");
    std::string method = msg["method"];
    json params = msg.value("params", json::object());
    if (!params.is_object()) params = json::object();

    if (is_notification) return nullptr;  // notifications/initialized, notifications/cancelled...: nothing to answer

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
                   meta.value("io.modelcontextprotocol/clientInfo", json::object()).dump().c_str(),
                   meta.value("io.modelcontextprotocol/clientCapabilities", json::object()).dump().c_str());
        Ctx modern = c;
        modern.era = tasks::Era::Modern;
        json versions = json::array({kModernVersion});
        for (const char *v : kSupportedVersions) versions.push_back(v);
        return rpc_result(id, json{{"supportedVersions", versions},
                                   {"capabilities", server_capabilities(modern)},
                                   {"instructions", g_instructions},
                                   {"ttlMs", 0},
                                   {"cacheScope", "private"},
                                   {"_meta", {{"io.modelcontextprotocol/serverInfo", server_info()}}}});
    }
    if (method == "ping") return rpc_result(id, json::object());
    if (method == "tools/list") return rpc_result(id, json{{"tools", tools_list_json(c)}, {"ttlMs", 0}, {"cacheScope", "private"}});
    if (method == "tools/call") {
        if (!params.contains("name") || !params["name"].is_string()) return rpc_error(id, -32602, "missing params.name");
        std::string name = params["name"];
        const Tool *t = find_tool(name);
        if (!t) return rpc_error(id, -32602, "unknown tool: " + name);
        json a = params.value("arguments", json::object());
        if (a.is_null()) a = json::object();
        bool legacy_task = c.era == tasks::Era::Legacy && params.contains("task") && params["task"].is_object();
        bool modern_task = client_supports_tasks(c) && server_wants_task(*t, a);
        if (legacy_task && (t->task_support.empty() || t->task_support == "forbidden"))
            return rpc_error(id, -32601, "tool " + name + " does not support task execution (execution.taskSupport absent)");
        if (legacy_task || modern_task) {
            std::string verr = validate_tool(*t, a);
            if (!verr.empty()) return rpc_result(id, to_call_result(agent::err("invalid_params", verr)));
            return unwrap_rpc(id, tasks::create_tool_task(name, a, params.value("task", json::object()), c.era));
        }
        if (c.era == tasks::Era::Legacy && t->task_support == "required")
            return rpc_error(id, -32600, "Task augmentation required for tools/call requests");
        return rpc_result(id, call_tool(name, a));
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
        if (res.is_null()) return rpc_error(id, -32002, "resource not found", json{{"uri", uri}});
        return rpc_result(id, json{{"contents", json::array({res})}});
    }
    if (c.era == tasks::Era::Modern) c.http_status = 404;  // modern era: unknown method = 404 + -32601
    return rpc_error(id, -32601, "unknown method: " + method);
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

// Decodes the "=?base64?...?=" sentinel of the Mcp-Name / Mcp-Param-* headers.
static std::string decode_header_value(const std::string &v) {
    const std::string pre = "=?base64?", suf = "?=";
    if (v.size() < pre.size() + suf.size() || v.compare(0, pre.size(), pre) != 0 ||
        v.compare(v.size() - suf.size(), suf.size(), suf) != 0)
        return v;
    std::string b = v.substr(pre.size(), v.size() - pre.size() - suf.size());
    static const std::string tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -8;
    for (char ch : b) {
        if (ch == '=') break;
        auto p = tbl.find(ch);
        if (p == std::string::npos) return v;
        val = ((val << 6) + (int)p) & 0xFFFFFF;
        bits += 6;
        if (bits >= 0) {
            out.push_back(char((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
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

static std::string handle_inner(const std::string &body, const Headers &headers, int &http_status);

// Safety net: any unexpected exception becomes a JSON-RPC internal error instead of an empty HTTP 500.
std::string handle(const std::string &body, const Headers &headers, int &http_status) {
    try {
        return handle_inner(body, headers, http_status);
    } catch (const std::exception &e) {
        http_status = 500;
        return rpc_error(nullptr, -32603, std::string("internal error: ") + e.what()).dump(-1, ' ', false, json::error_handler_t::replace);
    }
}

static std::string handle_inner(const std::string &body, const Headers &headers, int &http_status) {
    json msg;
    if (!nesting_ok(body, 64)) {
        http_status = 400;
        return rpc_error(nullptr, -32600, "JSON nesting too deep (max 64)").dump();
    }
    try {
        msg = json::parse(body);
    } catch (const std::exception &) {
        http_status = 400;
        return rpc_error(nullptr, -32700, "invalid JSON").dump();
    }
    if (msg.is_array()) {
        http_status = 400;
        return rpc_error(nullptr, -32600, "JSON-RPC batching is not supported").dump();
    }
    if (!msg.is_object()) {
        http_status = 400;
        return rpc_error(nullptr, -32600, "a JSON-RPC message must be an object").dump();
    }
    json id = msg.is_object() ? msg.value("id", json(nullptr)) : json(nullptr);
    Ctx c;
    std::string hv = header(headers, "mcp-protocol-version");
    json params = msg.is_object() && msg.contains("params") && msg["params"].is_object() ? msg["params"] : json::object();
    json meta = params.value("_meta", json::object());
    std::string mv = meta.is_object() ? meta.value("io.modelcontextprotocol/protocolVersion", "") : "";
    bool is_modern = hv == kModernVersion || !mv.empty();
    if (is_modern) {
        // 2026-07-28 era: the version must be supported.
        std::string ver = !mv.empty() ? mv : hv;
        if (ver != kModernVersion) {
            bool legacy_known = false;
            for (const char *v : kSupportedVersions) legacy_known = legacy_known || ver == v;
            if (!legacy_known) {
                http_status = 400;
                json sup = json::array({kModernVersion});
                for (const char *v : kSupportedVersions) sup.push_back(v);
                return rpc_error(id, -32022, "Unsupported protocol version: " + ver, json{{"supported", sup}, {"requested", ver}}).dump();
            }
            is_modern = false;
        }
    }
    if (is_modern) {
        std::string method = msg.value("method", "");
        auto mismatch = [&](const std::string &why) {
            http_status = 400;
            return rpc_error(id, -32020, "Header mismatch: " + why).dump();
        };
        if (hv.empty()) return mismatch("missing MCP-Protocol-Version header");
        if (!mv.empty() && hv != mv) return mismatch("MCP-Protocol-Version '" + hv + "' differs from _meta '" + mv + "'");
        std::string hm = header(headers, "mcp-method");
        if (hm.empty()) return mismatch("missing Mcp-Method header");
        if (hm != method) return mismatch("Mcp-Method '" + hm + "' differs from method '" + method + "'");
        if (method == "tools/call" || method == "resources/read" || method == "prompts/get") {
            std::string body_name = params.value(method == "resources/read" ? "uri" : "name", "");
            std::string hn = decode_header_value(header(headers, "mcp-name"));
            if (hn.empty()) return mismatch("missing Mcp-Name header");
            if (hn != body_name) return mismatch("Mcp-Name '" + hn + "' differs from '" + body_name + "'");
        }
        c.era = tasks::Era::Modern;
        c.client_caps = meta.value("io.modelcontextprotocol/clientCapabilities", json::object());
    }
    bool notif = false;
    json r = handle_message(msg, notif, c);
    if (notif) {
        http_status = 202;
        return "";
    }
    if (c.era == tasks::Era::Modern && r.contains("result") && r["result"].is_object() && !r["result"].contains("resultType"))
        r["result"]["resultType"] = "complete";
    http_status = c.http_status;
    return r.dump(-1, ' ', false, json::error_handler_t::replace);
}

}  // namespace mcp

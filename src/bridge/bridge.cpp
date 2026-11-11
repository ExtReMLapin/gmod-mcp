// gmod-mcp-bridge: a stdio MCP server that relays every message to the gmod-mcp server running inside Garry's Mod
// (http://127.0.0.1:<port>/mcp). An MCP client starts the bridge itself, so it connects even while the game is closed:
// the bridge then answers from the last lists it saw (or a minimal one), tool calls say the game is not running, and as
// soon as the game answers again the bridge sends list_changed so the client fetches the live tools.
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "platform/platform.h"
#include "util.h"

using json = nlohmann::json;

namespace {

const char *kBridgeVersion = GMOD_MCP_VERSION;
const char *kModernVersion = "2026-07-28";
const char *kMetaVersion = "io.modelcontextprotocol/protocolVersion";
const char *kSupported[] = {"2025-11-25", "2025-06-18", "2025-03-26"};

int g_port = 27080;
std::string g_dir;  // <user config>/gmod-mcp

// --- Output: one JSON message per line on stdout ---
std::mutex g_out_mu;
void out(const json &m) {
    std::string s = m.dump(-1, ' ', false, json::error_handler_t::replace);
    std::lock_guard<std::mutex> lk(g_out_mu);
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}
void log(const std::string &s) { std::fprintf(stderr, "gmod-mcp-bridge: %s\n", s.c_str()); }

json rpc_result(const json &id, json r) { return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(r)}}; }
json rpc_error(const json &id, int code, const std::string &msg) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", msg}}}};
}
json notification(const std::string &method) { return json{{"jsonrpc", "2.0"}, {"method", method}, {"params", json::object()}}; }

// --- Cache of the last answers from the game (instructions, lists), reused while it is closed ---
std::mutex g_cache_mu;
json g_cache = json::object();
std::string cache_path() { return g_dir + "/bridge-cache.json"; }
void cache_load() {
    std::string s;
    if (!plat::read_file(cache_path(), s)) return;
    try {
        json j = json::parse(s);
        if (j.is_object()) g_cache = j;
    } catch (...) {
    }
}
void cache_put(const std::string &key, const json &v) {
    std::lock_guard<std::mutex> lk(g_cache_mu);
    if (g_cache.value(key, json()) == v) return;
    g_cache[key] = v;
    plat::make_dirs(g_dir);
    plat::write_file(cache_path(), g_cache.dump());
}
json cache_get(const std::string &key) {
    std::lock_guard<std::mutex> lk(g_cache_mu);
    return g_cache.value(key, json());
}

// --- Connection state ---
enum class Link { Unknown, Up, Down };
std::atomic<Link> g_link{Link::Unknown};
std::mutex g_why_mu;
std::string g_why;  // why the game is unreachable
std::atomic<bool> g_initialized{false};
std::atomic<bool> g_listed{false};      // the client has fetched at least one list
std::string g_legacy_version;           // version negotiated by initialize (legacy era)
std::mutex g_state_mu;

std::string why_down() {
    std::lock_guard<std::mutex> lk(g_why_mu);
    return g_why;
}

std::string not_running_text() {
    return "Garry's Mod is not reachable on 127.0.0.1:" + std::to_string(g_port) + " (" + why_down() +
           "). Start the game with the -mcp launch option (the gmod-mcp skill knows how); the tools come back by "
           "themselves once it answers, no reconnection needed.";
}

// --- HTTP relay ---
std::mutex g_cancel_mu;
std::map<std::string, std::shared_ptr<std::atomic<bool>>> g_cancel;

bool needs_encoding(const std::string &v) {
    for (unsigned char c : v)
        if (c < 0x20 || c > 0x7e) return true;
    return !v.empty() && (v.front() == ' ' || v.back() == ' ');
}
std::string header_value(const std::string &v) { return needs_encoding(v) ? "=?base64?" + util::base64_encode(v) + "?=" : v; }

bool is_modern(const json &msg) {
    const json &p = msg.value("params", json::object());
    return p.is_object() && p.contains("_meta") && p["_meta"].is_object() && p["_meta"].contains(kMetaVersion);
}

struct Relay {
    bool reached = false;  // false: game unreachable (why in g_why)
    int status = 0;
    json response;         // final JSON-RPC response (null for notifications)
};

void set_down(const std::string &why) {
    {
        std::lock_guard<std::mutex> lk(g_why_mu);
        g_why = why;
    }
    g_link = Link::Down;
}

// Sends one message to the game. Notifications seen on an SSE response (progress...) are written to stdout as they
// arrive; the final response is returned.
Relay relay(const json &msg, double read_timeout_s, std::shared_ptr<std::atomic<bool>> cancel = nullptr, bool quiet = false) {
    Relay r;
    std::string token;
    if (!plat::read_file(g_dir + "/token", token)) {
        set_down("no token yet: the game has never run with -mcp on this machine");
        return r;
    }
    while (!token.empty() && (token.back() == '\n' || token.back() == '\r' || token.back() == ' ')) token.pop_back();

    httplib::Headers h{{"Authorization", "Bearer " + token}, {"Accept", "application/json, text/event-stream"}};
    std::string method = msg.value("method", "");
    if (is_modern(msg)) {
        const json &p = msg["params"];
        h.emplace("MCP-Protocol-Version", p["_meta"][kMetaVersion].get<std::string>());
        if (!method.empty()) h.emplace("Mcp-Method", method);
        if (method == "tools/call" || method == "prompts/get") h.emplace("Mcp-Name", header_value(p.value("name", "")));
        if (method == "resources/read") h.emplace("Mcp-Name", header_value(p.value("uri", "")));
        if (method == "tools/call" && p.contains("arguments") && p["arguments"].is_object() && p["arguments"].contains("instance") &&
            p["arguments"]["instance"].is_string())
            h.emplace("Mcp-Param-Instance", header_value(p["arguments"]["instance"].get<std::string>()));
    } else {
        std::lock_guard<std::mutex> lk(g_state_mu);
        if (!g_legacy_version.empty() && method != "initialize") h.emplace("MCP-Protocol-Version", g_legacy_version);
    }

    httplib::Client cli("127.0.0.1", g_port);
    cli.set_connection_timeout(1, 0);
    cli.set_read_timeout((time_t)read_timeout_s, 0);
    cli.set_write_timeout(10, 0);

    json id = msg.value("id", json());
    std::string body, pending;
    bool sse = false, decided = false;
    auto on_event = [&](const std::string &data) {
        json m;
        try {
            m = json::parse(data);
        } catch (...) {
            return;
        }
        if (!m.contains("method") && m.contains("id") && m["id"] == id) r.response = std::move(m);
        else if (!quiet) out(m);
    };
    auto receiver = [&](const char *data, size_t n) {
        if (cancel && cancel->load()) return false;
        if (!decided) {
            size_t i = 0;
            while (i < n && std::isspace((unsigned char)data[i])) ++i;
            if (i == n) return true;
            sse = data[i] != '{';
            decided = true;
        }
        if (!sse) {
            body.append(data, n);
            return true;
        }
        pending.append(data, n);
        size_t pos;
        // SSE: events are separated by a blank line; only the data lines matter here.
        while ((pos = pending.find("\n\n")) != std::string::npos || (pos = pending.find("\r\n\r\n")) != std::string::npos) {
            bool crlf = pending.compare(pos, 4, "\r\n\r\n") == 0;
            std::string ev = pending.substr(0, pos);
            pending.erase(0, pos + (crlf ? 4 : 2));
            std::string data_lines;
            size_t start = 0;
            while (start <= ev.size()) {
                size_t end = ev.find('\n', start);
                if (end == std::string::npos) end = ev.size();
                std::string line = ev.substr(start, end - start);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.rfind("data:", 0) == 0) {
                    std::string d = line.substr(5);
                    if (!d.empty() && d[0] == ' ') d.erase(0, 1);
                    if (!data_lines.empty()) data_lines += '\n';
                    data_lines += d;
                }
                start = end + 1;
            }
            if (!data_lines.empty()) on_event(data_lines);
        }
        return true;
    };
    std::string payload = msg.dump(-1, ' ', false, json::error_handler_t::replace);
    auto res = cli.Post("/mcp", h, payload, "application/json", receiver);
    if (!res) {
        if (cancel && cancel->load()) {
            r.reached = true;  // we closed it ourselves
            return r;
        }
        auto e = res.error();
        if (e == httplib::Error::Connection || e == httplib::Error::ConnectionTimeout)
            set_down("nothing listening: the game is closed or was started without -mcp");
        else
            set_down("connection error: " + httplib::to_string(e));
        return r;
    }
    r.reached = true;
    r.status = res->status;
    if (res->status == 401) {
        r.reached = false;
        set_down("the game rejected the token in " + g_dir + "/token");
        return r;
    }
    g_link = Link::Up;
    if (!sse && !body.empty()) {
        try {
            json m = json::parse(body);
            if (m.is_object()) r.response = std::move(m);
        } catch (...) {
        }
    }
    return r;
}

// Folder of this executable.
std::string exe_dir() {
#ifdef _WIN32
    char buf[MAX_PATH * 4] = {};
    DWORD n = GetModuleFileNameA(nullptr, buf, (DWORD)sizeof buf);
    std::string p(buf, n);
    for (auto &c : p)
        if (c == '\\') c = '/';
#else
    char buf[PATH_MAX] = {};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    std::string p(buf, n > 0 ? (size_t)n : 0);
#endif
    size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? "." : p.substr(0, slash);
}

// The plugin ships the module next to the bridge: <plugin>/bin/gmod-mcp-bridge and <plugin>/module/<platform>/garrysmod/...
std::string module_dir() { return exe_dir() + "/../module/" + plat::platform_tag(); }

// --- Answers built by the bridge ---
json patch_capabilities(json result) {
    if (!result.is_object()) return result;
    json &caps = result["capabilities"];
    if (!caps.is_object()) caps = json::object();
    for (const char *k : {"tools", "prompts", "resources"}) {
        if (!caps.contains(k) || !caps[k].is_object()) caps[k] = json::object();
        caps[k]["listChanged"] = true;  // the bridge announces the live lists when the game comes back
    }
    return result;
}

std::string pick_version(const std::string &asked) {
    for (const char *v : kSupported)
        if (asked == v) return v;
    return kSupported[0];
}

std::string offline_instructions() {
    return "gmod-mcp drives a local Garry's Mod instance. The game is not running right now: start it with the -mcp "
           "launch option (the gmod-mcp skill knows how). The tools and the full guide (resource mcpagent://guide) come "
           "back by themselves once it answers.";
}

json fallback_list(const std::string &method) {
    if (method == "tools/list") {
        json status{{"name", "status"},
                    {"title", "Instance status"},
                    {"description", "Status of the Garry's Mod instance. The game is not running: this tells how to start it."},
                    {"inputSchema", {{"type", "object"}, {"properties", json::object()}}}};
        return json{{"tools", json::array({status})}};
    }
    if (method == "prompts/list") return json{{"prompts", json::array()}};
    if (method == "resources/list") return json{{"resources", json::array()}};
    return json{{"resourceTemplates", json::array()}};
}

std::string list_key(const json &msg) { return msg.value("method", "") + (is_modern(msg) ? "|modern" : "|legacy"); }

void handle_request(const json &msg) {
    json id = msg["id"];
    std::string method = msg.value("method", "");
    std::string key = id.dump();
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> lk(g_cancel_mu);
        g_cancel[key] = cancel;
    }
    bool is_list = method == "tools/list" || method == "prompts/list" || method == "resources/list" || method == "resources/templates/list";
    Relay r = relay(msg, method == "tools/call" ? 4000.0 : 60.0, cancel);
    {
        std::lock_guard<std::mutex> lk(g_cancel_mu);
        g_cancel.erase(key);
    }
    if (r.reached && cancel->load() && r.response.is_null()) return;  // cancelled by the client

    if (method == "initialize" || method == "server/discover") {
        g_initialized = true;
        json res;
        if (r.reached && r.response.contains("result")) {
            res = r.response["result"];
            cache_put(method, res);
            // The module in the game and this bridge come from the same release: say so when they do not.
            std::string mod = res.value("serverInfo", json::object()).value("version", "");
            if (!mod.empty() && mod != kBridgeVersion && res.contains("instructions") && res["instructions"].is_string()) {
                res["instructions"] = "Version mismatch: the gmod-mcp module in the game is " + mod + ", this plugin ships " +
                                      kBridgeVersion + ". Tell the user, and offer to update the module with the gmod-mcp skill "
                                      "(the game must be quit first).\n\n" + res["instructions"].get<std::string>();
                log("module " + mod + " in the game, bridge " + kBridgeVersion);
            }
        } else {
            res = cache_get(method);
            if (!res.is_object()) {
                res = json{{"capabilities", json::object()},
                           {"serverInfo", {{"name", "gmod-mcp"}, {"version", kBridgeVersion}}},
                           {"instructions", offline_instructions()}};
                if (method == "server/discover") res["supportedVersions"] = json::array({kModernVersion, kSupported[0], kSupported[1], kSupported[2]});
            } else if (res.contains("instructions") && res["instructions"].is_string()) {
                res["instructions"] = "The game is not running right now: start it with -mcp; the tools come back by themselves.\n\n" +
                                      res["instructions"].get<std::string>();
            }
            if (method == "initialize") res["protocolVersion"] = pick_version(msg["params"].value("protocolVersion", ""));
        }
        if (method == "initialize" && res.contains("protocolVersion") && res["protocolVersion"].is_string()) {
            std::lock_guard<std::mutex> lk(g_state_mu);
            g_legacy_version = res["protocolVersion"];
        }
        out(rpc_result(id, patch_capabilities(res)));
        return;
    }
    if (is_list) g_listed = true;
    if (r.reached && !r.response.is_null()) {
        if (is_list && r.response.contains("result")) cache_put(list_key(msg), r.response["result"]);
        out(r.response);
        return;
    }
    if (r.reached) {
        out(rpc_error(id, -32603, "the game answered HTTP " + std::to_string(r.status) + " without a JSON-RPC response"));
        return;
    }
    // Game unreachable.
    if (is_list) {
        json cached = cache_get(list_key(msg));
        out(rpc_result(id, cached.is_object() ? cached : fallback_list(method)));
    } else if (method == "ping") {
        out(rpc_result(id, json::object()));
    } else if (method == "tools/call") {
        json text{{"type", "text"}, {"text", not_running_text()}};
        out(rpc_result(id, json{{"content", json::array({text})},
                                {"structuredContent", {{"error", {{"code", "game_not_running"}, {"message", not_running_text()}}}}},
                                {"isError", true}}));
    } else {
        out(rpc_error(id, -32603, not_running_text()));
    }
}

void handle_notification(const json &msg) {
    std::string method = msg.value("method", "");
    if (method == "notifications/cancelled") {
        const json &p = msg.value("params", json::object());
        if (p.is_object() && p.contains("requestId")) {
            std::lock_guard<std::mutex> lk(g_cancel_mu);
            auto it = g_cancel.find(p["requestId"].dump());
            if (it != g_cancel.end()) it->second->store(true);
        }
    }
    relay(msg, 5.0, nullptr, true);  // best effort (the game may be closed)
}

// Watches the game: when it comes back after being unreachable, asks the client to fetch the lists again.
void watcher() {
    Link last = g_link.load();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (!g_initialized) continue;
        Relay r = relay(json{{"jsonrpc", "2.0"}, {"id", "gmod-mcp-bridge-probe"}, {"method", "ping"}}, 3.0, nullptr, true);
        Link now = r.reached ? Link::Up : Link::Down;
        if (now != last) {
            log(now == Link::Up ? "the game answers" : "the game is unreachable: " + why_down());
            if (now == Link::Up && g_listed) {
                for (const char *n : {"notifications/tools/list_changed", "notifications/prompts/list_changed",
                                      "notifications/resources/list_changed"})
                    out(notification(n));
            }
            last = now;
        }
    }
}

}  // namespace

int main(int argc, char **argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (const char *p = std::getenv("GMOD_MCP_PORT")) g_port = std::atoi(p);
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) g_port = std::atoi(argv[++i]);
        else if (a == "--version") {
            std::printf("gmod-mcp-bridge %s\n", kBridgeVersion);
            return 0;
        } else if (a == "--module-dir") {
            // Folder holding garrysmod/lua/bin/... of the module shipped with this bridge (copied into the game folder).
            std::printf("%s\n", module_dir().c_str());
            return 0;
        }
    }
    if (g_port <= 0 || g_port > 65535) g_port = 27080;
    g_dir = plat::user_config_dir() + "/gmod-mcp";
    cache_load();
    std::thread(watcher).detach();

    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find_first_not_of(" \t") == std::string::npos) continue;
        json msg;
        try {
            msg = json::parse(line);
        } catch (...) {
            out(rpc_error(nullptr, -32700, "invalid JSON"));
            continue;
        }
        if (!msg.is_object()) {
            out(rpc_error(nullptr, -32600, "JSON-RPC batching is not supported"));
            continue;
        }
        if (!msg.contains("method")) continue;  // a response: this server never sends requests
        if (msg.contains("id")) std::thread(handle_request, msg).detach();
        else std::thread(handle_notification, msg).detach();
    }
    std::fflush(stdout);
    std::_Exit(0);  // detached threads may still be waiting on the game
}

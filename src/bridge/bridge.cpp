// gmod-mcp-bridge: a stdio MCP server that relays every message to the gmod-mcp server running inside Garry's Mod
// (http://127.0.0.1:<port>/mcp). An MCP client starts the bridge itself, so it connects even while the game is closed:
// the bridge then answers from the last lists it saw (or a minimal one), tool calls say the game is not running, and as
// soon as the game answers again the bridge sends list_changed so the client fetches the live tools.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <tlhelp32.h>
#else
#include <limits.h>
#include <fcntl.h>
#include <sys/file.h>
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
const char *kMetaCaps = "io.modelcontextprotocol/clientCapabilities";
const char *kMetaServerInfo = "io.modelcontextprotocol/serverInfo";
const char *kMetaSubscription = "io.modelcontextprotocol/subscriptionId";
const char *kSupported[] = {"2025-11-25", "2025-06-18", "2025-03-26"};
const double kNoIdleLimit = 7 * 24 * 3600.0;

int g_port = 27080;
bool g_watch_parent = true;  // --no-parent-watch: for launchers that exit while the client keeps the pipes
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
// Lock shared by every bridge of this user (one per client session) around the cache read-merge-write. The OS releases
// it when a bridge dies, so a crash never leaves a stale lock.
class CacheLock {
#ifdef _WIN32
    HANDLE h_ = INVALID_HANDLE_VALUE;

  public:
    CacheLock() {
        std::string p = g_dir + "/bridge-cache.lock";
        h_ = CreateFileA(p.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
        OVERLAPPED ov{};
        if (h_ != INVALID_HANDLE_VALUE && !LockFileEx(h_, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &ov)) {
            CloseHandle(h_);
            h_ = INVALID_HANDLE_VALUE;
        }
    }
    ~CacheLock() {
        if (h_ == INVALID_HANDLE_VALUE) return;
        OVERLAPPED ov{};
        UnlockFileEx(h_, 0, 1, 0, &ov);
        CloseHandle(h_);
    }
#else
    int fd_ = -1;

  public:
    CacheLock() {
        fd_ = ::open((g_dir + "/bridge-cache.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd_ >= 0 && ::flock(fd_, LOCK_EX) != 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    ~CacheLock() {
        if (fd_ < 0) return;
        ::flock(fd_, LOCK_UN);
        ::close(fd_);
    }
#endif
};

// Several bridges share the file: under the lock, each write merges into what is on disk and replaces the file in one
// rename, so a reader never sees a half-written file and no bridge loses another's entries.
void cache_put(const std::string &key, const json &v) {
    std::lock_guard<std::mutex> lk(g_cache_mu);
    if (g_cache.value(key, json()) == v) return;
    plat::make_dirs(g_dir);
    CacheLock file_lock;
    std::string s;
    if (plat::read_file(cache_path(), s)) {
        try {
            json disk = json::parse(s);
            if (disk.is_object()) g_cache = disk;
        } catch (...) {
        }
    }
    g_cache[key] = v;
    std::string tmp = cache_path() + "." + std::to_string(plat::process_id()) + ".tmp";
    if (!plat::write_file(tmp, g_cache.dump())) return;
    std::error_code ec;
    std::filesystem::rename(std::filesystem::u8path(tmp), std::filesystem::u8path(cache_path()), ec);
    if (ec) plat::remove_file(tmp);
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
std::atomic<bool> g_served_offline{false};  // the bridge answered with an offline list or description
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
// A request in flight: cancelling it closes its HTTP connection right away, even while the stream is silent.
struct Inflight {
    std::atomic<bool> cancelled{false};
    bool legacy = true;  // a request of a pre-2026-07-28 era (cancellation is also forwarded as a notification)
    std::mutex mu;
    httplib::Client *cli = nullptr;
    void cancel() {
        cancelled = true;
        std::lock_guard<std::mutex> lk(mu);
        if (cli) cli->stop();
    }
};
std::mutex g_cancel_mu;
std::map<std::string, std::shared_ptr<Inflight>> g_cancel;

bool needs_encoding(const std::string &v) {
    for (unsigned char c : v)
        if (c < 0x20 || c > 0x7e) return true;
    return !v.empty() && (v.front() == ' ' || v.back() == ' ');
}
std::string header_value(const std::string &v) { return needs_encoding(v) ? "=?base64?" + util::base64_encode(v) + "?=" : v; }

// params as an object (absent or of another type: empty object).
json params_of(const json &msg) {
    auto it = msg.find("params");
    return it != msg.end() && it->is_object() ? *it : json::object();
}

bool is_modern(const json &msg) {
    const json &p = msg.value("params", json::object());
    return p.is_object() && p.contains("_meta") && p["_meta"].is_object() && p["_meta"].contains(kMetaVersion) &&
           p["_meta"][kMetaVersion].is_string();
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

using NoteFn = std::function<void(const json &)>;

// Sends one message to the game. Notifications seen on an SSE response (progress...) go to on_note as they arrive;
// the final response is returned.
Relay relay(const json &msg, double read_timeout_s, std::shared_ptr<Inflight> cancel = nullptr, NoteFn on_note = nullptr) {
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
        else if (on_note) on_note(m);
    };
    auto receiver = [&](const char *data, size_t n) {
        if (cancel && cancel->cancelled) return false;
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
    if (cancel) {
        std::lock_guard<std::mutex> lk(cancel->mu);
        if (cancel->cancelled) {
            r.reached = true;
            return r;
        }
        cancel->cli = &cli;
    }
    // A refused connection right after the game answered is retried briefly (a burst of calls, a busy accept queue);
    // a closed game still fails within half a second.
    bool was_up = g_link == Link::Up;
    auto res = cli.Post("/mcp", h, payload, "application/json", receiver);
    for (int delay_ms : {100, 300}) {
        if (res || !was_up || res.error() != httplib::Error::Connection || decided || (cancel && cancel->cancelled)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        res = cli.Post("/mcp", h, payload, "application/json", receiver);
    }
    if (cancel) {
        std::lock_guard<std::mutex> lk(cancel->mu);
        cancel->cli = nullptr;
    }
    if (!res) {
        if (cancel && cancel->cancelled) {
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

json all_versions() { return json::array({kModernVersion, kSupported[0], kSupported[1], kSupported[2]}); }

// 2026-07-28 requests answered by the bridge itself (game closed, subscriptions): the checks the game would make.
// Returns an error response, or null when the request is acceptable.
json modern_error(const json &msg) {
    json id = msg.value("id", json());
    json meta = params_of(msg).value("_meta", json::object());
    if (!meta.is_object() || !meta.contains(kMetaVersion) || !meta[kMetaVersion].is_string())
        return rpc_error(id, -32602, std::string("missing _meta ") + kMetaVersion);
    std::string v = meta[kMetaVersion];
    bool known = v == kModernVersion;
    for (const char *s : kSupported) known = known || v == s;
    if (!known) {
        json e = rpc_error(id, -32022, "Unsupported protocol version: " + v);
        e["error"]["data"] = json{{"supported", all_versions()}, {"requested", v}};
        return e;
    }
    if (v == kModernVersion && (!meta.contains(kMetaCaps) || !meta[kMetaCaps].is_object()))
        return rpc_error(id, -32602, std::string("missing _meta ") + kMetaCaps);
    return nullptr;
}

bool modern_2026(const json &msg) {
    return is_modern(msg) && params_of(msg)["_meta"][kMetaVersion].get<std::string>() == kModernVersion;
}

// A result built by the bridge: in the 2026-07-28 era it says it is complete and who answered.
json local_result(const json &msg, json r) {
    if (modern_2026(msg)) {
        r["resultType"] = "complete";
        if (!r.contains("_meta") || !r["_meta"].is_object()) r["_meta"] = json::object();
        r["_meta"][kMetaServerInfo] = json{{"name", "gmod-mcp"}, {"version", kBridgeVersion}};
    }
    return rpc_result(msg["id"], std::move(r));
}

std::string offline_instructions() {
    return "gmod-mcp drives a local Garry's Mod instance. The game is not running right now: start it with the -mcp "
           "launch option (the gmod-mcp skill knows how). The tools and the full guide (resource mcpagent://guide) come "
           "back by themselves once it answers.";
}

json fallback_list(const std::string &method) {
    json r;
    if (method == "tools/list") {
        json status{{"name", "status"},
                    {"title", "Instance status"},
                    {"description", "Status of the Garry's Mod instance. The game is not running: this tells how to start it."},
                    {"inputSchema", {{"type", "object"}, {"properties", json::object()}}},
                    {"annotations", {{"title", "Instance status"}, {"readOnlyHint", true}, {"openWorldHint", false}}}};
        r = json{{"tools", json::array({status})}};
    } else if (method == "prompts/list") {
        r = json{{"prompts", json::array()}};
    } else if (method == "resources/list") {
        r = json{{"resources", json::array()}};
    } else {
        r = json{{"resourceTemplates", json::array()}};
    }
    return r;
}

std::string list_key(const json &msg) { return msg.value("method", "") + (is_modern(msg) ? "|modern" : "|legacy"); }

// --- Subscriptions (2026-07-28): handled by the bridge, so list_changed reaches the client when the game comes back.
// Resource subscriptions are relayed to the game, and reopened when it restarts.
struct Sub {
    json id;
    json meta;  // _meta of the listen request, reused for the requests to the game
    bool tools = false, prompts = false, resources = false;
    json uris = json::array();  // resource subscriptions honored
    std::shared_ptr<Inflight> upstream = std::make_shared<Inflight>();
    std::atomic<bool> ended{false};
    std::mutex mu;
    std::condition_variable cv;
};
std::mutex g_subs_mu;
std::map<std::string, std::shared_ptr<Sub>> g_subs;
std::atomic<bool> g_client_legacy{false};  // the client used initialize: list_changed may be sent on their own

json sub_meta(const Sub &s) { return json{{kMetaSubscription, s.id}}; }

void sub_notify(const Sub &s, const std::string &method, json params = json::object()) {
    if (s.ended) return;
    params["_meta"] = sub_meta(s);
    out(json{{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}});
}

// Opens the subscription to the game for the honored resources, forwards its updates, and reopens it after the game
// restarts. Returns when the client cancels.
void sub_upstream_loop(std::shared_ptr<Sub> s) {
    while (!s->ended) {
        if (s->uris.empty()) break;
        json up{{"jsonrpc", "2.0"}, {"id", s->id}, {"method", "subscriptions/listen"},
                {"params", {{"_meta", s->meta}, {"notifications", {{"resourceSubscriptions", s->uris}}}}}};
        std::shared_ptr<Inflight> f;
        {
            std::lock_guard<std::mutex> lk(s->mu);
            if (s->ended) break;
            s->upstream = f = std::make_shared<Inflight>();
        }
        relay(up, kNoIdleLimit, f, [s](const json &m) {
            if (m.value("method", "") == "notifications/resources/updated") {
                json p = m.value("params", json::object());
                p.erase("_meta");
                sub_notify(*s, "notifications/resources/updated", p);
            }
            // notifications/subscriptions/acknowledged: the client already has the bridge's.
        });
        if (s->ended) break;
        std::unique_lock<std::mutex> lk(s->mu);
        s->cv.wait_for(lk, std::chrono::seconds(2), [&] { return s->ended.load(); });
    }
}

void handle_listen(const json &msg) {
    json err = modern_error(msg);
    if (!err.is_null()) {
        out(err);
        return;
    }
    auto s = std::make_shared<Sub>();
    s->id = msg["id"];
    json p = params_of(msg);
    s->meta = p["_meta"];
    json f = p.value("notifications", json::object());
    if (!f.is_object()) f = json::object();
    json honored = json::object();
    auto flag = [&](const char *k, bool &dst) {
        if (f.value(k, false) == true) dst = true, honored[k] = true;
    };
    flag("toolsListChanged", s->tools);
    flag("promptsListChanged", s->prompts);
    flag("resourcesListChanged", s->resources);
    if (f.contains("resourceSubscriptions") && f["resourceSubscriptions"].is_array()) {
        // Ask the game which ones it honors; while it is closed, use what it said last time.
        json asked = json::array();
        for (const auto &u : f["resourceSubscriptions"])
            if (u.is_string()) asked.push_back(u);
        json known = cache_get("subscribable");
        for (const auto &u : asked)
            if (known.is_array() && std::find(known.begin(), known.end(), u) != known.end()) s->uris.push_back(u);
        if (g_link != Link::Down && !asked.empty()) {
            json probe{{"jsonrpc", "2.0"}, {"id", "gmod-mcp-bridge-listen-probe"}, {"method", "subscriptions/listen"},
                       {"params", {{"_meta", s->meta}, {"notifications", {{"resourceSubscriptions", asked}}}}}};
            auto inflight = std::make_shared<Inflight>();
            json ack;
            relay(probe, 5.0, inflight, [&](const json &m) {
                if (m.value("method", "") == "notifications/subscriptions/acknowledged") {
                    ack = m;
                    std::thread([inflight] { inflight->cancel(); }).detach();
                }
            });
            if (ack.is_object()) {
                json got = ack["params"].value("notifications", json::object()).value("resourceSubscriptions", json::array());
                // Remember every URI the game ever honored (it only grows: they are fixed by the module).
                json all = known.is_array() ? known : json::array();
                for (const auto &u : got)
                    if (std::find(all.begin(), all.end(), u) == all.end()) all.push_back(u);
                cache_put("subscribable", all);
                s->uris = got;
            }
        }
        honored["resourceSubscriptions"] = s->uris;
    }
    {
        std::lock_guard<std::mutex> lk(g_subs_mu);
        g_subs[s->id.dump()] = s;
    }
    sub_notify(*s, "notifications/subscriptions/acknowledged", json{{"notifications", honored}});
    sub_upstream_loop(s);  // returns at once without resource subscriptions
    // The subscription now lives until the client cancels it (no response is sent for a cancelled request).
}

void end_subscription(const std::string &key) {
    std::shared_ptr<Sub> s;
    {
        std::lock_guard<std::mutex> lk(g_subs_mu);
        auto it = g_subs.find(key);
        if (it == g_subs.end()) return;
        s = it->second;
        g_subs.erase(it);
    }
    s->ended = true;
    {
        std::lock_guard<std::mutex> lk(s->mu);
        s->upstream->cancel();
    }
    s->cv.notify_all();
}

void announce_lists_changed() {
    if (g_client_legacy) {
        for (const char *n : {"notifications/tools/list_changed", "notifications/prompts/list_changed", "notifications/resources/list_changed"})
            out(notification(n));
    }
    std::vector<std::shared_ptr<Sub>> subs;
    {
        std::lock_guard<std::mutex> lk(g_subs_mu);
        for (auto &kv : g_subs) subs.push_back(kv.second);
    }
    for (auto &s : subs) {
        if (s->tools) sub_notify(*s, "notifications/tools/list_changed");
        if (s->prompts) sub_notify(*s, "notifications/prompts/list_changed");
        if (s->resources) sub_notify(*s, "notifications/resources/list_changed");
    }
}

void handle_request(const json &msg) {
    json id = msg["id"];
    std::string method = msg.value("method", "");
    g_initialized = true;
    if (method == "initialize") g_client_legacy = true;
    if (method == "subscriptions/listen" && is_modern(msg)) {
        handle_listen(msg);
        return;
    }
    std::string key = id.dump();
    auto cancel = std::make_shared<Inflight>();
    cancel->legacy = !modern_2026(msg);
    {
        std::lock_guard<std::mutex> lk(g_cancel_mu);
        g_cancel[key] = cancel;
    }
    bool is_list = method == "tools/list" || method == "prompts/list" || method == "resources/list" || method == "resources/templates/list";
    // No idle limit on client requests: a tool call or tasks/result may stay silent for a long time. The client ends
    // them by cancelling (the bridge then closes the HTTP request).
    Relay r = relay(msg, kNoIdleLimit, cancel, [](const json &m) { out(m); });
    {
        std::lock_guard<std::mutex> lk(g_cancel_mu);
        g_cancel.erase(key);
    }
    if (cancel->cancelled) return;  // cancelled by the client: no response (late answers are dropped too)

    if (method == "initialize" || method == "server/discover") {
        json res;
        if (r.reached && r.response.contains("result")) {
            res = r.response["result"];
            json stored = res;
            stored.erase("_meta");
            cache_put(method, stored);
            // The module in the game and this bridge come from the same release: say so when they do not.
            std::string mod = res.value("serverInfo", json::object()).value("version", "");
            if (mod.empty() && res.contains("_meta") && res["_meta"].is_object())
                mod = res["_meta"].value(kMetaServerInfo, json::object()).value("version", "");
            if (!mod.empty() && mod != kBridgeVersion && res.contains("instructions") && res["instructions"].is_string()) {
                res["instructions"] = "Version mismatch: the gmod-mcp module in the game is " + mod + ", this plugin ships " +
                                      kBridgeVersion + ". Tell the user, and offer to update the module with the gmod-mcp skill "
                                      "(the game must be quit first).\n\n" + res["instructions"].get<std::string>();
                log("module " + mod + " in the game, bridge " + kBridgeVersion);
            }
            if (method == "initialize") {
                std::lock_guard<std::mutex> lk(g_state_mu);
                if (res.contains("protocolVersion") && res["protocolVersion"].is_string()) g_legacy_version = res["protocolVersion"];
            }
            json full = r.response;
            full["result"] = patch_capabilities(res);
            out(full);
            return;
        }
        if (r.reached && !r.response.is_null()) {  // an error from the game (unsupported version...)
            out(r.response);
            return;
        }
        if (method == "server/discover") {
            json err = modern_error(msg);
            if (!err.is_null()) {
                out(err);
                return;
            }
        }
        res = cache_get(method);
        if (!res.is_object()) {
            res = json{{"capabilities", json::object()},
                       {"serverInfo", {{"name", "gmod-mcp"}, {"version", kBridgeVersion}}},
                       {"instructions", offline_instructions()}};
            if (method == "server/discover") res["supportedVersions"] = all_versions();
        } else if (res.contains("instructions") && res["instructions"].is_string()) {
            res["instructions"] = "The game is not running right now: start it with -mcp; the tools come back by themselves.\n\n" +
                                  res["instructions"].get<std::string>();
        }
        if (method == "server/discover") res["ttlMs"] = 0;  // do not keep this offline answer
        if (method == "initialize") {
            res["protocolVersion"] = pick_version(params_of(msg).value("protocolVersion", ""));
            std::lock_guard<std::mutex> lk(g_state_mu);
            g_legacy_version = res["protocolVersion"];
        }
        g_served_offline = true;
        out(local_result(msg, patch_capabilities(res)));
        return;
    }
    if (is_list) g_listed = true;
    if (r.reached && !r.response.is_null()) {
        if (is_list && r.response.contains("result")) {
            json stored = r.response["result"];
            stored.erase("_meta");
            stored.erase("resultType");
            cache_put(list_key(msg), stored);
        }
        out(r.response);
        return;
    }
    if (r.reached) {
        out(rpc_error(id, -32603, "the game answered HTTP " + std::to_string(r.status) + " without a JSON-RPC response"));
        return;
    }
    // Game unreachable: the bridge answers, with the checks the game would make.
    if (is_modern(msg)) {
        json err = modern_error(msg);
        if (!err.is_null()) {
            out(err);
            return;
        }
    }
    if (is_list) {
        json cached = cache_get(list_key(msg));
        json res = cached.is_object() ? cached : fallback_list(method);
        res["ttlMs"] = 0;  // the live list replaces it as soon as the game answers
        res["cacheScope"] = "private";
        g_served_offline = true;
        out(local_result(msg, res));
    } else if (method == "ping") {
        out(local_result(msg, json::object()));
    } else if (method == "tools/call") {
        json text{{"type", "text"}, {"text", not_running_text()}};
        out(local_result(msg, json{{"content", json::array({text})},
                                   {"structuredContent", {{"error", {{"code", "game_not_running"}, {"message", not_running_text()}}}}},
                                   {"isError", true}}));
    } else {
        out(rpc_error(id, -32603, not_running_text()));
    }
}

void handle_notification(const json &msg) {
    std::string method = msg.value("method", "");
    bool forward = true;
    if (method == "notifications/cancelled") {
        json p = params_of(msg);
        if (p.contains("requestId")) {
            std::string key = p["requestId"].dump();
            end_subscription(key);  // a subscription the bridge holds: nothing to forward
            std::shared_ptr<Inflight> f;
            {
                std::lock_guard<std::mutex> lk(g_cancel_mu);
                auto it = g_cancel.find(key);
                if (it != g_cancel.end()) f = it->second;
            }
            // Closing the HTTP stream is the cancellation for the game; a legacy-era request also gets the notification.
            forward = f && f->legacy;
            if (f) f->cancel();
        } else {
            forward = false;
        }
    }
    if (forward) relay(msg, 5.0);  // best effort (the game may be closed)
}

// Watches the game: when it comes back after being unreachable, the client is asked to fetch the lists again.
void watcher() {
    Link last = g_link.load();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (!g_initialized) continue;
        Relay r;
        try {
            r = relay(json{{"jsonrpc", "2.0"}, {"id", "gmod-mcp-bridge-probe"}, {"method", "ping"}}, 3.0);
        } catch (const std::exception &e) {
            log(std::string("probe failed: ") + e.what());
            continue;
        }
        Link now = r.reached ? Link::Up : Link::Down;
        if (now != last) {
            log(now == Link::Up ? "the game answers" : "the game is unreachable: " + why_down());
            if (now == Link::Up && (last == Link::Down || g_served_offline.exchange(false))) announce_lists_changed();
            last = now;
        }
    }
}

// Orphan guard. Some clients leave their stdio servers running when the session ends, or the pipe breaks without an
// end-of-file: an orphaned bridge would keep probing the game forever. The bridge exits when the process that started
// it is gone.
#ifdef _WIN32
DWORD parent_pid() {
    DWORD me = GetCurrentProcessId(), parent = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 e{};
    e.dwSize = sizeof e;
    for (BOOL ok = Process32First(snap, &e); ok; ok = Process32Next(snap, &e))
        if (e.th32ProcessID == me) {
            parent = e.th32ParentProcessID;
            break;
        }
    CloseHandle(snap);
    return parent;
}
void watch_parent() {
    DWORD pid = parent_pid();
    HANDLE h = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
    if (!h) return;  // unknown parent (already gone or no access): rely on end-of-file
    std::thread([h] {
        WaitForSingleObject(h, INFINITE);
        log("the client process is gone: exiting");
        std::_Exit(0);
    }).detach();
}
#else
void watch_parent() {
    pid_t parent = getppid();
    std::thread([parent] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (getppid() != parent) {  // reparented: the client is gone
                log("the client process is gone: exiting");
                std::_Exit(0);
            }
        }
    }).detach();
}
#endif

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
        else if (a == "--no-parent-watch") g_watch_parent = false;
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
    if (g_watch_parent) watch_parent();
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
        // An exception escaping a thread would end the whole bridge: each message fails on its own instead.
        std::thread([msg] {
            try {
                if (msg.contains("id")) handle_request(msg);
                else handle_notification(msg);
            } catch (const std::exception &e) {
                log(std::string("internal error: ") + e.what());
                if (msg.contains("id")) out(rpc_error(msg["id"], -32603, std::string("gmod-mcp-bridge internal error: ") + e.what()));
            }
        }).detach();
    }
    std::fflush(stdout);
    std::_Exit(0);  // detached threads may still be waiting on the game
}

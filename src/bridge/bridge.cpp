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
#include <regex>
#include <set>
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
#include <signal.h>
#include <sys/prctl.h>
#include <unistd.h>
#endif

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "argcheck.h"
#include "bridge_snapshot.h"
#include "mcp_protocol.h"
#include "platform/platform.h"
#include "lua_mount.h"
#include "util.h"

using json = nlohmann::json;

namespace {

const char *kBridgeVersion = GMOD_MCP_VERSION;
const char *kModernVersion = mcpproto::kModern;
const char *kMetaVersion = mcpproto::kMetaProtocolVersion;
const char *kMetaCaps = mcpproto::kMetaClientCapabilities;
const char *kMetaServerInfo = mcpproto::kMetaServerInfo;
const char *kMetaSubscription = mcpproto::kMetaSubscriptionId;
const auto &kSupported = mcpproto::kLegacy;
namespace err = mcpproto::err;
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
    // Leading/trailing spaces, and a plain value that looks like the sentinel itself, are encoded too (2026-07-28).
    return !v.empty() && (v.front() == ' ' || v.back() == ' ' || v.rfind("=?base64?", 0) == 0);
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
        return rpc_error(id, err::kInvalidParams, std::string("missing _meta ") + kMetaVersion);
    std::string v = meta[kMetaVersion];
    bool known = v == kModernVersion;
    for (const char *s : kSupported) known = known || v == s;
    if (!known) {
        json e = rpc_error(id, err::kUnsupportedProtocolVersion, "Unsupported protocol version: " + v);
        e["error"]["data"] = json{{"supported", all_versions()}, {"requested", v}};
        return e;
    }
    if (v == kModernVersion && (!meta.contains(kMetaCaps) || !meta[kMetaCaps].is_object()))
        return rpc_error(id, err::kInvalidParams, std::string("missing _meta ") + kMetaCaps);
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

// The module's offline description, embedded at build time (same release as this bridge): what is served while the
// game has never answered on this machine, so a fresh install or a registry's build sees every tool.
const json &snapshot() {
    static const json s = [] {
        try {
            return json::parse(reinterpret_cast<const char *>(bridge_snapshot), reinterpret_cast<const char *>(bridge_snapshot) + bridge_snapshot_size);
        } catch (...) {
            return json::object();
        }
    }();
    return s;
}

std::string offline_instructions() {
    return "gmod-mcp drives a local Garry's Mod instance. The game is not running right now: start it with the -mcp "
           "launch option (the gmod-mcp skill knows how). The tools and the full guide (resource mcpagent://guide) come "
           "back by themselves once it answers.";
}

json fallback_list(const std::string &method, bool modern) {
    const json &snap = snapshot();
    const char *key = method == "tools/list" ? (modern ? "tools_modern" : "tools_legacy")
                      : method == "prompts/list" ? "prompts"
                      : method == "resources/list" ? "resources"
                                                   : "resourceTemplates";
    const char *field = method == "tools/list" ? "tools"
                        : method == "prompts/list" ? "prompts"
                        : method == "resources/list" ? "resources"
                                                     : "resourceTemplates";
    if (snap.contains(key) && snap[key].is_array()) return json{{field, snap[key]}};
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
        if (!known.is_array()) known = snapshot().value("subscribable", json::array());
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

// --- The bridge's own tools: test clients it launches and closes, and the session's default instance. They live in
// the bridge because it runs before any game does (the module cannot start the first one) and holds one session.
struct Launched {
    uint32_t pid = 0;
    std::string id;
    std::string owner;  // free label given at launch (a sub-agent's name): instances_close {owner} closes only those
    bool keep = false;
#ifdef _WIN32
    HANDLE h = nullptr;
#endif
};
std::mutex g_launch_mu;
std::vector<Launched> g_launched;
std::mutex g_default_mu;
std::string g_default_instance;
std::mutex g_itools_mu;
std::set<std::string> g_instance_tools;  // game tools that take an `instance` argument

void learn_instance_tools(const json &tools) {
    if (!tools.is_array()) return;
    std::lock_guard<std::mutex> lk(g_itools_mu);
    for (const auto &t : tools) {
        const json &props = t.value("inputSchema", json::object()).value("properties", json::object());
        if (props.is_object() && props.contains("instance")) g_instance_tools.insert(t.value("name", ""));
    }
}

bool takes_instance(const std::string &tool) {
    std::lock_guard<std::mutex> lk(g_itools_mu);
    return g_instance_tools.count(tool) > 0;
}

const char *kLaunchName = "instances_launch";
const char *kCloseName = "instances_close";
const char *kSelectName = "instance_select";

json bridge_tools() {
    json launch{
        {"name", kLaunchName},
        {"title", "Launch test clients"},
        {"description",
         "Starts 1 to 8 extra Garry's Mod clients for tests, and works while no game runs (the first one then becomes the "
         "main instance). Each starts with -multirun, gmod-mcp on, a small window and a frame cap, waits until it answers, "
         "then loads `map` through map_load (never +map: a game loading a map in the background can grab the mouse). "
         "Returns their instance ids and pids: pass an id as `instance` on every call. "
         "They close when this session ends unless keep is true; close them earlier with instances_close (not game_quit: "
         "a clean quit writes their window size over the user's own config). Sub-agents of one Claude Code session share "
         "this bridge: give each one's clients an `owner` label and close them with instances_close {owner}, never "
         "{all: true}, which would close the other sub-agents' clients too. The game is found through Steam; pass "
         "game_dir (the folder holding gmod_win64.exe) if it is elsewhere. Windows only for now.\n"
         "Presets: 'light' (default: 640x360, 20 fps; for most tests, NOT for performance measurements: the 20 fps cap "
         "hides per-frame cost) and 'normal' (640x360, 30 fps; for rendering checks, screenshots and frame-time sensitive "
         "tests). sound: false (-nosound) changes what the engine reports about sounds: SoundDuration of compressed "
         "(ADPCM) WAV files differs (0.49 s instead of 1.24 s seen); keep sound on for sound tests.\n"
         "All clients share the game folder with the user's own game: same addons, cfg, data/ and cookies (sv.db/cl.db). "
         "A test that writes cookies, data files or archived convars changes the user's game too: clean up after it.\n"
         "Tell the user beforehand: the game saves the window size of these clients as its own video setting, so their next "
         "normal start of Garry's Mod opens at that size until they set their resolution back in the video options.\n"
         "Measured cost per client on gm_construct (16-core PC, the game and its 4 browser subprocesses): light about 19% "
         "of one core and 1.5 GB of RAM; normal 24% and 1.5 GB. Eight clients: about 1.5 cores and 12 GB. Memory is the "
         "limit, not CPU. The frame cap is what saves CPU (30 to 20 fps: -20%); sound: false saves about 12% more. Window "
         "size barely matters, and a client narrower than 640 pixels crashes while loading a map (integer division by zero "
         "in client.dll at client Lua startup: seen at 480x270 and 320x240; it only survived with the HUD turned off). "
         "-textmode and lower graphics settings were measured: no real gain (and graphics settings would be saved into the "
         "user's own), so neither is offered."},
        {"inputSchema",
         {{"type", "object"},
          {"properties",
           {{"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 8}, {"description", "How many clients (default 1)."}}},
            {"map", {{"type", "string"}, {"pattern", "^[A-Za-z0-9_.-]{1,64}$"}, {"description", "Map to load on each, e.g. gm_construct. Default: stay at the main menu."}}},
            {"maxplayers", {{"type", "integer"}, {"minimum", 1}, {"maximum", 128}, {"description", "For map_load (default 1: singleplayer)."}}},
            {"preset", {{"type", "string"}, {"enum", json::array({"light", "normal"})}, {"description", "Default light."}}},
            {"sound", {{"type", "boolean"}, {"description", "Keep sound on (default true). false saves CPU when sound does not matter."}}},
            {"keep", {{"type", "boolean"}, {"description", "Keep them running after this session ends (default false)."}}},
            {"owner", {{"type", "string"}, {"maxLength", 64}, {"description", "Label recorded with these clients (e.g. your sub-agent's name or task); instances_close {owner} then closes only them."}}},
            {"lua_mount", {{"type", "string"}, {"maxLength", 1024},
                         {"description", "Absolute folder whose Lua these clients load before every addon's (e.g. your git worktree of "
                                         "the addon: its root or its lua/ folder). Applies from their first map load; see lua_mount. "
                                         "Windows only."}}},
            {"game_dir", {{"type", "string"}, {"description", "Game folder (holding gmod_win64.exe), if Steam does not know it."}}},
            {"timeout_s", {{"type", "number"}, {"minimum", 10}, {"maximum", 900}, {"description", "Max wait for all of them, map included (default 180)."}}}}},
          {"additionalProperties", false}}},
        {"annotations", {{"title", "Launch test clients"}, {"readOnlyHint", false}, {"destructiveHint", false}, {"openWorldHint", false}}}};
    json close{
        {"name", kCloseName},
        {"title", "Close test clients"},
        {"description",
         "Closes clients this session started with instances_launch, at once and without writing their config, by "
         "instance id, by pid (also for a client that never answered, e.g. gmod-mcp did not load in it) or by owner label. "
         "Other games (the user's own, another session's) are refused: close those only if the user asks, with "
         "game_terminate. Sub-agents of one Claude Code session share it: all: true closes their clients too."},
        {"inputSchema",
         {{"type", "object"},
          {"properties",
           {{"instances", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Instance ids to close."}}},
            {"pids", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"description", "Process ids to close (as returned by instances_launch)."}}},
            {"owner", {{"type", "string"}, {"description", "Close every client launched with this owner label."}}},
            {"all", {{"type", "boolean"}, {"description", "Close every client this session started, other sub-agents' included."}}}}},
          {"additionalProperties", false}}},
        {"annotations", {{"title", "Close test clients"}, {"readOnlyHint", false}, {"destructiveHint", true}, {"openWorldHint", false}}}};
    json select{
        {"name", kSelectName},
        {"title", "Set the default instance"},
        {"description",
         "Makes `instance` this session's default: tool calls without an `instance` argument go to it (useful with several "
         "games running, where they would otherwise be refused as ambiguous). null clears it. Lasts until the session ends. "
         "Do not use it with sub-agents: they all share this session, so a default set by one applies to every other's "
         "calls too (they would act on its game). Pass `instance` on every call instead."},
        {"inputSchema",
         {{"type", "object"},
          {"properties", {{"instance", {{"type", json::array({"string", "null"})}, {"description", "Instance id (see instances_list), or null."}}}}},
          {"required", json::array({"instance"})},
          {"additionalProperties", false}}},
        {"annotations", {{"title", "Set the default instance"}, {"readOnlyHint", false}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}}};
    return json::array({launch, close, select});
}

bool is_bridge_tool(const std::string &name) { return name == kLaunchName || name == kCloseName || name == kSelectName; }

json tool_text(json structured, bool error);

// instances_list, completed by the bridge: the owner label of each client it launched, the clients it launched that
// never answered (gmod-mcp not loaded, still starting), and an optional owner filter. `sc` is the game's answer
// (or an empty list when no game answers).
json annotate_instances(json sc, const json &args) {
    std::string want = args.value("owner", "");
    std::set<uint32_t> listed;
    json kept = json::array();
    std::lock_guard<std::mutex> lk(g_launch_mu);
    for (auto e : sc.value("instances", json::array())) {
        uint32_t pid = e.value("pid", 0u);
        listed.insert(pid);
        for (const auto &l : g_launched)
            if (l.pid == pid) {
                if (!l.owner.empty()) e["owner"] = l.owner;
                e["launched_by_this_session"] = true;
            }
        if (!want.empty() && e.value("owner", "") != want) continue;
        kept.push_back(std::move(e));
    }
    sc["instances"] = kept;
    json silent = json::array();
    for (const auto &l : g_launched) {
        if (listed.count(l.pid)) continue;
        if (!want.empty() && l.owner != want) continue;
#ifdef _WIN32
        if (l.h && WaitForSingleObject(l.h, 0) == WAIT_OBJECT_0) continue;  // exited
#endif
        json x{{"pid", l.pid}, {"note", "launched by this session, not answering: still starting, or gmod-mcp did not load in it "
                                        "(see garrysmod/data/mcpagent/last_crash.txt). instances_close {pids} closes it."}};
        if (!l.owner.empty()) x["owner"] = l.owner;
        silent.push_back(x);
    }
    if (!silent.empty()) sc["launched_not_answering"] = silent;
    if (!want.empty()) sc["owner_filter"] = want;
    return sc;
}

json tool_text(json structured, bool error) {
    std::string t = structured.dump(-1, ' ', false, json::error_handler_t::replace);
    return json{{"content", json::array({json{{"type", "text"}, {"text", t}}})}, {"structuredContent", std::move(structured)}, {"isError", error}};
}
json tool_error(const std::string &code, const std::string &message) {
    return tool_text(json{{"error", {{"code", code}, {"message", message}}}}, true);
}

// A tool of the game, called by the bridge itself (legacy era, no stream). Returns its structuredContent, or
// {"error": ...} when the game is unreachable or the call failed.
json game_call(const std::string &tool, json args, double timeout_s) {
    json m{{"jsonrpc", "2.0"}, {"id", "gmod-mcp-bridge-internal"}, {"method", "tools/call"},
           {"params", {{"name", tool}, {"arguments", std::move(args)}}}};
    Relay r = relay(m, timeout_s);
    if (!r.reached) return json{{"error", {{"code", "game_not_running"}, {"message", why_down()}}}};
    if (!r.response.contains("result")) return json{{"error", {{"code", "rpc_error"}, {"message", r.response.value("error", json::object()).value("message", "no result")}}}};
    return r.response["result"].value("structuredContent", json::object());
}

std::string parent_of(std::string p) {
    while (!p.empty() && (p.back() == '/' || p.back() == '\\')) p.pop_back();
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(0, s);
}

bool file_is(const std::string &p) {
    std::error_code ec;
    return std::filesystem::is_regular_file(std::filesystem::u8path(p), ec);
}

// The game folder: the argument, then what the module reported (status: game_dir), then Steam's libraries.
[[maybe_unused]] std::string find_game_root(const std::string &arg, std::string &how) {
    auto ok = [](std::string d) {
        while (!d.empty() && (d.back() == '/' || d.back() == '\\')) d.pop_back();
        std::string low = d;
        for (auto &c : low) c = (char)std::tolower((unsigned char)c);
        if (low.size() >= 9 && low.compare(low.size() - 9, 9, "garrysmod") == 0 && !file_is(d + "/gmod_win64.exe")) d = parent_of(d);
        return file_is(d + "/gmod_win64.exe") ? d : std::string();
    };
    if (!arg.empty()) {
        how = "game_dir argument";
        return ok(arg);
    }
    json cached = cache_get("game_root");
    if (cached.is_string() && !ok(cached.get<std::string>()).empty()) {
        how = "reported by the game earlier";
        return ok(cached.get<std::string>());
    }
#ifdef _WIN32
    char buf[1024] = {};
    DWORD n = sizeof buf;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", RRF_RT_REG_SZ, nullptr, buf, &n) == ERROR_SUCCESS) {
        std::string steam = buf;
        std::vector<std::string> libs{steam};
        std::string vdf;
        if (plat::read_file(steam + "/steamapps/libraryfolders.vdf", vdf)) {
            std::regex re("\"path\"\\s+\"([^\"]+)\"");
            for (std::sregex_iterator it(vdf.begin(), vdf.end(), re), end; it != end; ++it) {
                std::string p = (*it)[1];
                std::string un;
                for (size_t i = 0; i < p.size(); ++i) {
                    if (p[i] == '\\' && i + 1 < p.size() && p[i + 1] == '\\') ++i;
                    un += p[i];
                }
                libs.push_back(un);
            }
        }
        for (const auto &l : libs) {
            std::string d = ok(l + "/steamapps/common/GarrysMod");
            if (!d.empty()) {
                how = "Steam library " + l;
                return d;
            }
        }
    }
#endif
    how = "not found";
    return "";
}

[[maybe_unused]] void progress(const json &token, double done, double total, const std::string &message) {
    if (token.is_null()) return;
    json p{{"progressToken", token}, {"progress", done}, {"message", message}};
    if (total > 0) p["total"] = total;
    out(json{{"jsonrpc", "2.0"}, {"method", "notifications/progress"}, {"params", p}});
}

json launch_tool(const json &args, const json &token) {
#ifndef _WIN32
    (void)args;
    (void)token;
    return tool_error("unsupported", "instances_launch is Windows only for now: on Linux start the client with steam -applaunch 4000 -mcp -multirun ...");
#else
    int count = args.value("count", 1);
    std::string preset = args.value("preset", "light");
    bool sound = args.value("sound", true), keep = args.value("keep", false);
    double timeout = args.value("timeout_s", 180.0);
    std::string map = args.value("map", "");
    std::string owner = args.value("owner", "");
    // Checked before anything starts: a bad folder must not leave clients behind.
    std::string mount_root;
    if (args.contains("lua_mount") && args["lua_mount"].is_string() && !args["lua_mount"].get<std::string>().empty()) {
        std::string oerr;
        if (!lua_mount::normalize_root(args["lua_mount"].get<std::string>(), mount_root, oerr)) return tool_error("invalid_params", "lua_mount: " + oerr);
    }
    std::string how, root = find_game_root(args.value("game_dir", ""), how);
    if (root.empty())
        return tool_error("game_not_found", "Garry's Mod not found (" + how + "): pass game_dir, the folder holding gmod_win64.exe (the gmod-mcp skill explains how to find it)");
    if (!mount_root.empty()) {
        std::string inside = lua_mount::refuse_inside_game(mount_root, root + "/garrysmod");
        if (!inside.empty()) return tool_error("invalid_params", "lua_mount: " + inside);
    }
    // Fixed option set (no free arguments). The light preset comes from measurements of what actually saves CPU and
    // memory per client.
    std::string opts = "-multirun -mcp -windowed -novid +engine_no_focus_sleep 0 +snd_mute_losefocus 0";
    if (preset == "normal") opts += " -w 640 -h 360 +fps_max 30 +fps_max_nofocus 30";
    // No graphics options: the video ones (-dxlevel, mat_*) are saved into the user's own settings, for little gain.
    else opts += " -w 640 -h 360 +fps_max 20 +fps_max_nofocus 20";  // narrower than 640 crashes the client
    if (!sound) opts += " -nosound";
    // The folder was checked: no quote or line break can end the argument.
    if (!mount_root.empty()) opts += " -mcp_lua_mount \"" + mount_root + "\"";
    SetEnvironmentVariableA("SteamAppId", "4000");
    SetEnvironmentVariableA("SteamGameId", "4000");
    std::string exe = root + "\\gmod_win64.exe";
    for (auto &c : exe)
        if (c == '/') c = '\\';
    std::vector<Launched> started;
    for (int i = 0; i < count; ++i) {
        std::string cmd = "\"" + exe + "\" " + opts;
        std::wstring wcmd(cmd.begin(), cmd.end()), wdir(root.begin(), root.end());
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE, 0, nullptr, wdir.c_str(), &si, &pi)) {
            for (auto &l : started) TerminateProcess(l.h, 0);
            return tool_error("launch_failed", "CreateProcess failed (error " + std::to_string(GetLastError()) + ") for " + exe);
        }
        CloseHandle(pi.hThread);
        Launched l;
        l.pid = pi.dwProcessId;
        l.h = pi.hProcess;
        l.keep = keep;
        l.owner = owner;
        started.push_back(l);
    }
    {
        std::lock_guard<std::mutex> lk(g_launch_mu);
        for (auto &l : started) g_launched.push_back(l);
    }
    progress(token, 0, count, std::to_string(count) + " client(s) started, waiting for them to answer");
    // Wait until each one has registered with the main instance (instances_list gives its id and port).
    auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    json found = json::object();
    while ((int)found.size() < count && elapsed() < timeout) {
        json lst = game_call("instances_list", json::object(), 5);
        for (const auto &in : lst.value("instances", json::array()))
            for (auto &l : started)
                if (in.value("pid", 0u) == l.pid && !found.contains(std::to_string(l.pid))) {
                    found[std::to_string(l.pid)] = in;
                    l.id = in.value("id", "");
                    progress(token, (double)found.size(), count, l.id + " answers");
                }
        for (auto &l : started)
            if (WaitForSingleObject(l.h, 0) == WAIT_OBJECT_0 && !found.contains(std::to_string(l.pid)))
                return tool_error("launch_failed", "client pid " + std::to_string(l.pid) + " exited during startup (Steam not running? see garrysmod/console.log)");
        if ((int)found.size() < count) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    {
        std::lock_guard<std::mutex> lk(g_launch_mu);
        for (auto &g : g_launched)
            for (auto &l : started)
                if (g.pid == l.pid) g.id = l.id;
    }
    if ((int)found.size() < count) {
        json silent = json::array(), answered = json::array();
        for (auto &l : started)
            if (!found.contains(std::to_string(l.pid))) silent.push_back(l.pid);
            else answered.push_back(json{{"instance", l.id}, {"pid", l.pid}});
        return tool_text(json{{"error", {{"code", "timeout"},
                                         {"message", std::to_string(found.size()) + " of " + std::to_string(count) +
                                                         " clients answered within " + std::to_string((int)timeout) +
                                                         " s (still starting, or gmod-mcp did not load in them: see "
                                                         "garrysmod/data/mcpagent/last_crash.txt and the console). Close "
                                                         "the silent ones with instances_close {pids}."}}},
                              {"silent_pids", silent}, {"answered", answered},
                              {"lua_mount", mount_root.empty() ? json(nullptr) : json(mount_root)}},
                         true);
    }
    // Maps, all at once.
    json results = json::array();
    std::vector<json> map_res(started.size());
    if (!map.empty()) {
        progress(token, 0, count, "loading " + map);
        std::vector<std::thread> th;
        for (size_t i = 0; i < started.size(); ++i)
            th.emplace_back([&, i] {
                json a{{"map", map}, {"instance", started[i].id}, {"timeout_s", std::max(30.0, timeout - elapsed())}};
                if (args.contains("maxplayers")) a["maxplayers"] = args["maxplayers"];
                map_res[i] = game_call("map_load", a, std::max(40.0, timeout - elapsed() + 10));
            });
        for (auto &t : th) t.join();
    }
    bool all_ok = true;
    for (size_t i = 0; i < started.size(); ++i) {
        const json &in = found[std::to_string(started[i].pid)];
        json e{{"instance", started[i].id}, {"pid", started[i].pid}, {"port", in.value("port", 0)}};
        if (!owner.empty()) e["owner"] = owner;
        if (!map.empty()) {
            bool ok = !map_res[i].contains("error");
            all_ok = all_ok && ok;
            e["map"] = ok ? json(map) : json(nullptr);
            if (!ok) e["map_error"] = map_res[i]["error"];
        }
        results.push_back(e);
    }
    int fps_cap = preset == "normal" ? 30 : 20;
    json res{{"instances", results}, {"game_dir", root}, {"found_by", how}, {"preset", preset}, {"keep", keep},
             {"fps_max", fps_cap},
             {"lua_mount", mount_root.empty() ? json(nullptr) : json(mount_root)},
             {"fps_note", "these clients run at most " + std::to_string(fps_cap) + " fps (fps_max, also when unfocused): "
                          "per-frame costs and frame-to-frame behaviour differ from a normal game; raise it with "
                          "convar_set fps_max for timing work"},
             {"next", "Pass `instance` to target one, or instance_select to make one the default; instances_close when done."}};
    return tool_text(res, !all_ok);
#endif
}

json close_tool(const json &args) {
    bool all = args.value("all", false);
    json ids = args.value("instances", json::array());
    json pids = args.value("pids", json::array());
    std::string owner = args.value("owner", "");
    if (!all && ids.empty() && pids.empty() && owner.empty())
        return tool_error("invalid_params", "pass instances: [ids], pids: [pids], owner: \"label\" or all: true");
    std::vector<Launched> todo;
    json refused = json::array();
    {
        std::lock_guard<std::mutex> lk(g_launch_mu);
        if (all) {
            todo = g_launched;
            g_launched.clear();
        } else {
            auto take = [&](auto pred) {
                auto it = std::find_if(g_launched.begin(), g_launched.end(), pred);
                if (it == g_launched.end()) return false;
                todo.push_back(*it);
                g_launched.erase(it);
                return true;
            };
            for (const auto &idj : ids) {
                std::string id = idj.is_string() ? idj.get<std::string>() : "";
                if (!take([&](const Launched &l) { return !id.empty() && l.id == id; })) refused.push_back(id);
            }
            for (const auto &pj : pids) {
                uint32_t pid = pj.is_number_unsigned() ? pj.get<uint32_t>() : 0;
                if (!take([&](const Launched &l) { return pid != 0 && l.pid == pid; })) refused.push_back(pj);
            }
            if (!owner.empty()) {
                bool any = false;
                while (take([&](const Launched &l) { return l.owner == owner; })) any = true;
                if (!any) refused.push_back("owner:" + owner);
            }
        }
    }
    json closed = json::array();
    for (auto &l : todo) {
        if (!l.id.empty()) game_call("game_terminate", json{{"instance", l.id}}, 5);
#ifdef _WIN32
        if (WaitForSingleObject(l.h, 3000) != WAIT_OBJECT_0) TerminateProcess(l.h, 0);
        CloseHandle(l.h);
#endif
        json c{{"pid", l.pid}};
        if (!l.id.empty()) c["instance"] = l.id;
        if (!l.owner.empty()) c["owner"] = l.owner;
        closed.push_back(c);
        std::lock_guard<std::mutex> lk(g_default_mu);
        if (g_default_instance == l.id) g_default_instance.clear();
    }
    json res{{"closed", closed}};
    if (!refused.empty()) {
        res["refused"] = refused;
        res["why_refused"] = "not started by this session (or already closed): close a game this session did not start only if the user asks (game_terminate)";
    }
    return tool_text(res, !refused.empty() && closed.empty());
}

json select_tool(const json &args) {
    std::lock_guard<std::mutex> lk(g_default_mu);
    std::string prev = g_default_instance;
    const json &v = args.contains("instance") ? args["instance"] : json();
    g_default_instance = v.is_string() ? v.get<std::string>() : "";
    return tool_text(json{{"default_instance", g_default_instance.empty() ? json(nullptr) : json(g_default_instance)},
                          {"previous", prev.empty() ? json(nullptr) : json(prev)}},
                     false);
}

// Closes the clients this session launched (unless kept), when the session ends.
void close_launched_on_exit() {
    std::lock_guard<std::mutex> lk(g_launch_mu);
    for (auto &l : g_launched) {
        if (l.keep) continue;
#ifdef _WIN32
        TerminateProcess(l.h, 0);  // immediate: like game_terminate, nothing is written to the user's config
#endif
    }
    g_launched.clear();
}

void handle_bridge_tool(const json &msg) {
    json p = params_of(msg);
    std::string name = p.value("name", "");
    json args = p.value("arguments", json::object());
    if (!args.is_object()) args = json::object();
    json token = p.value("_meta", json::object()).value("progressToken", json());
    // Arguments are checked against the tool's own schema first: an unchecked count 0, lua_mount 5 or preset "turbo" used
    // to be ignored or defaulted, and a launch went ahead anyway.
    for (const auto &t : bridge_tools())
        if (t.value("name", "") == name) {
            std::string e = argcheck::check(t.value("inputSchema", json::object()), p.contains("arguments") ? p["arguments"] : json::object());
            if (!e.empty()) {
                out(local_result(msg, tool_error("invalid_params", e)));
                return;
            }
        }
    json res;
    if (name == kLaunchName) res = launch_tool(args, token);
    else if (name == kCloseName) res = close_tool(args);
    else res = select_tool(args);
    out(local_result(msg, res));
}

// Tool lists the bridge serves: the game's, plus its own tools.
void add_bridge_tools(json &result) {
    if (!result.is_object() || !result.contains("tools") || !result["tools"].is_array()) return;
    for (const auto &t : bridge_tools()) result["tools"].push_back(t);
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
    if (modern_2026(msg) && (method == "initialize" || method == "ping")) {  // removed in 2026-07-28
        out(rpc_error(id, err::kMethodNotFound, std::string("method not found in protocol version ") + kModernVersion + ": " + method));
        return;
    }
    std::string tool = method == "tools/call" ? params_of(msg).value("name", "") : "";
    if (is_bridge_tool(tool)) {
        if (is_modern(msg)) {
            json e = modern_error(msg);
            if (!e.is_null()) {
                out(e);
                return;
            }
        }
        handle_bridge_tool(msg);
        return;
    }
    json fwd = msg;
    if (tool == "instances_list" && fwd["params"].contains("arguments") && fwd["params"]["arguments"].is_object())
        fwd["params"]["arguments"].erase("owner");
    if (!tool.empty() && takes_instance(tool)) {
        std::lock_guard<std::mutex> lk(g_default_mu);
        json &a = fwd["params"]["arguments"];
        if (!a.is_object()) a = json::object();
        if (!g_default_instance.empty() && !a.contains("instance")) a["instance"] = g_default_instance;
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
    Relay r = relay(fwd, kNoIdleLimit, cancel, [](const json &m) { out(m); });
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
        // The embedded description first: the cached one names the last game's instance and paths, stale by now.
        const json &snap = snapshot();
        res = snap.contains("serverInfo") ? json() : cache_get(method);
        if (!res.is_object() && snap.contains("serverInfo")) {
            res = json{{"serverInfo", snap["serverInfo"]}, {"instructions", snap.value("instructions", offline_instructions())},
                       {"capabilities", snap.value(method == "initialize" ? "capabilities_legacy" : "capabilities_modern", json::object())}};
            if (method == "server/discover") res["supportedVersions"] = snap.value("supportedVersions", all_versions());
            res["instructions"] = "The game is not running right now: start it with -mcp; the tools come back by themselves.\n\n" +
                                  res["instructions"].get<std::string>();
        } else if (!res.is_object()) {
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
            if (method == "tools/list") {
                learn_instance_tools(stored.value("tools", json::array()));
                add_bridge_tools(r.response["result"]);
            }
        }
        if (tool == "instances_list" && r.response.contains("result") && !r.response["result"].value("isError", false)) {
            json &res = r.response["result"];
            res = tool_text(annotate_instances(res.value("structuredContent", json::object()), params_of(msg).value("arguments", json::object())), false);
        }
        // Remember where the game is (instances_launch can then start more without asking).
        if (tool == "status" && r.response.contains("result")) {
            json sc = r.response["result"].value("structuredContent", json::object());
            if (sc.contains("game_dir") && sc["game_dir"].is_string()) cache_put("game_root", parent_of(sc["game_dir"].get<std::string>()));
        }
        out(r.response);
        return;
    }
    if (r.reached) {
        out(rpc_error(id, err::kInternalError, "the game answered HTTP " + std::to_string(r.status) + " without a JSON-RPC response"));
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
        json res = cached.is_object() ? cached : fallback_list(method, modern_2026(msg));
        res["ttlMs"] = 0;  // the live list replaces it as soon as the game answers
        res["cacheScope"] = "private";
        if (method == "tools/list") add_bridge_tools(res);
        g_served_offline = true;
        out(local_result(msg, res));
    } else if (method == "ping") {
        out(local_result(msg, json::object()));
    } else if (method == "tools/call" && tool == "instances_list") {
        json sc{{"instances", json::array()},
                {"note", "no game instance is running: nothing answers on 127.0.0.1:" + std::to_string(g_port) + " (" + why_down() +
                         "). instances_launch starts test clients; the user's game needs the -mcp launch option."}};
        out(local_result(msg, tool_text(annotate_instances(sc, params_of(msg).value("arguments", json::object())), false)));
    } else if (method == "tools/call") {
        json text{{"type", "text"}, {"text", not_running_text()}};
        out(local_result(msg, json{{"content", json::array({text})},
                                   {"structuredContent", {{"error", {{"code", "game_not_running"}, {"message", not_running_text()}}}}},
                                   {"isError", true}}));
    } else {
        out(rpc_error(id, err::kInternalError, not_running_text()));
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
// Taken first thing in main, before the parent has a chance to exit.
HANDLE g_parent = nullptr;
void capture_parent() {
    DWORD pid = parent_pid();
    g_parent = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;  // null: already gone or no access (rely on EOF)
}
void watch_parent() {
    HANDLE h = g_parent;
    if (!h) return;
    std::thread([h] {
        WaitForSingleObject(h, INFINITE);
        log("the client process is gone: exiting");
        close_launched_on_exit();
        std::_Exit(0);
    }).detach();
}
#else
pid_t g_parent = 0;
void capture_parent() {
    g_parent = getppid();
    // The kernel signals the bridge when its parent dies (no race from here on); the poll below is a fallback.
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() != g_parent) std::_Exit(0);  // the parent died between the two calls
}
void watch_parent() {
    pid_t parent = g_parent;
    std::thread([parent] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (getppid() != parent) {  // reparented: the client is gone
                log("the client process is gone: exiting");
                close_launched_on_exit();
                std::_Exit(0);
            }
        }
    }).detach();
}
#endif

}  // namespace

int main(int argc, char **argv) {
    bool watch = true;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--no-parent-watch") watch = false;
    if (watch) capture_parent();
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
    learn_instance_tools(snapshot().value("tools_legacy", json::array()));
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
            out(rpc_error(nullptr, err::kParseError, "invalid JSON"));
            continue;
        }
        if (!msg.is_object()) {
            out(rpc_error(nullptr, err::kInvalidRequest, "JSON-RPC batching is not supported"));
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
                if (msg.contains("id")) out(rpc_error(msg["id"], err::kInternalError, std::string("gmod-mcp-bridge internal error: ") + e.what()));
            }
        }).detach();
    }
    std::fflush(stdout);
    close_launched_on_exit();
    std::_Exit(0);  // detached threads may still be waiting on the game
}

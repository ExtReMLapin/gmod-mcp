// gmod-mcp-bridge: a stdio MCP server that relays every message to the gmod-mcp server running inside Garry's Mod
// (http://127.0.0.1:<port>/mcp). An MCP client starts the bridge itself, so it connects even while the game is closed:
// the bridge then answers from the last lists it saw (or a minimal one), tool calls say the game is not running, and as
// soon as the game answers again the bridge sends list_changed so the client fetches the live tools.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
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
#include "crashinfo.h"
#include "gamedir.h"
#include "launch_opts.h"  // Issue #10
#include "fp7137.h"  // FP7137
#include "gmod_mcp_reply.h"
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
std::atomic<bool> g_conflict{false};  // issue #5: the base port is held by a program that is not gmod-mcp (g_why says what answered)
std::atomic<int> g_found_port{0};     // issue #5: a gmod-mcp found on a later port while in conflict (0: none, or not looked for yet)
std::atomic<bool> g_initialized{false};
std::atomic<bool> g_listed{false};      // the client has fetched at least one list
std::atomic<bool> g_served_offline{false};  // the bridge answered with an offline list or description
std::string g_legacy_version;           // version negotiated by initialize (legacy era)
std::mutex g_state_mu;

std::string why_down() {
    std::lock_guard<std::mutex> lk(g_why_mu);
    return g_why;
}

// The port_conflict error (issue #5): another program answers on the port, so the game is not "not running": it may be
// running fine, on another port.
std::string port_conflict_text() {
    std::string port = std::to_string(g_port);
    return "127.0.0.1:" + port + " is held by another program, not by gmod-mcp (" + why_down() + ")" +
           (g_found_port ? "; a gmod-mcp answers on 127.0.0.1:" + std::to_string(g_found_port) + ": set GMOD_MCP_PORT=" + std::to_string(g_found_port) : "") +
           ". Find it: Get-NetTCPConnection -LocalPort " + port + " (Windows PowerShell) or ss -ltnp 'sport = :" + port +
           "' (Linux). Free the port, or set the GMOD_MCP_PORT environment variable to the port the game took (its "
           "console line `[mcpagent] active: ... http://127.0.0.1:<port>/mcp` says which) and restart the MCP client. "
           "See the README section \"Configuration\" and the gmod-mcp skill section \"When it still does not answer\".";
}

// The code of the error answered when the game cannot be reached.
const char *unreachable_code() { return g_conflict ? "port_conflict" : "game_not_running"; }

// What the server instructions say first while the game cannot be reached.
std::string offline_prefix() {
    if (g_conflict) return "port_conflict: the game may be running, but this bridge cannot reach it. " + port_conflict_text() + "\n\n";
    return "The game is not running right now: start it with -mcp; the tools come back by themselves.\n\n";
}

std::string not_running_text() {
    if (g_conflict) return "port_conflict: " + port_conflict_text();
    return "Garry's Mod is not reachable on 127.0.0.1:" + std::to_string(g_port) + " (" + why_down() +
           "). Start the game with the -mcp launch option (the gmod-mcp skill knows how); the tools come back by "
           "themselves once it answers, no reconnection needed.";
}

// --- HTTP relay ---
// A request in flight: cancelling it closes its HTTP connection right away, even while the stream is silent.
struct Inflight {
    std::atomic<bool> cancelled{false};
    std::atomic<bool> process_ended{false};  // issue #8: the launched client this call waits on died: the connection was cut to answer at once
    bool legacy = true;  // a request of a pre-2026-07-28 era (cancellation is also forwarded as a notification)
    std::mutex mu;
    httplib::Client *cli = nullptr;
    void cancel() {
        cancelled = true;
        std::lock_guard<std::mutex> lk(mu);
        if (cli) cli->stop();
    }
    void abort_for_process_end() {
        process_ended = true;
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
    bool refused = false;  // unreachable because the connection was never made (refused, or no answer to the connect): nothing was sent
    int status = 0;
    json response;         // final JSON-RPC response (null for notifications)
    bool aborted = false;  // issue #8: cut because the launched client the call waited on died (reached is true: the game itself is fine)
};

void set_down(const std::string &why) {
    {
        std::lock_guard<std::mutex> lk(g_why_mu);
        g_why = why;
    }
    g_conflict = false;
    g_found_port = 0;
    g_link = Link::Down;
}

// Issue #5: the port answers, but not as gmod-mcp. Looks at the next ports for a real gmod-mcp (the module takes the next
// free one when the base port is busy) so that the message can name it. Returns its port, or 0. Slow (a refused loopback
// connection can take 300 ms on Windows): runs on a background thread, see probe_next_ports.
int find_gmod_mcp_port(const std::string &token) {
    for (int p = g_port + 1; p <= g_port + 15 && p <= 65535; ++p) {
        httplib::Client c("127.0.0.1", p);
        c.set_connection_timeout(0, 300000);
        c.set_read_timeout(1, 0);
        c.set_write_timeout(1, 0);
        auto r = c.Post("/mcp", httplib::Headers{{"Authorization", "Bearer " + token}},
                        R"({"jsonrpc":"2.0","id":"gmod-mcp-bridge-port-probe","method":"ping"})", "application/json");
        if (r && gmcp::is_gmod_mcp_reply(r->has_header(gmcp::kSignatureHeader), r->body)) return p;
    }
    return 0;
}

// At most one probe at a time, at most every 10 s: the answer is read by the next error message.
void probe_next_ports(const std::string &token) {
    static std::atomic<bool> running{false};
    static std::atomic<long long> last_ms{-100000};
    long long now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - last_ms < 10000 || running.exchange(true)) return;
    last_ms = now;
    std::thread([token] {
        int p = find_gmod_mcp_port(token);
        if (g_conflict) g_found_port = p;
        running = false;
    }).detach();
}

void set_port_conflict(int status, const std::string &token) {
    int found = g_found_port;  // what the last probe found stays while the conflict lasts
    set_down("it answered HTTP " + std::to_string(status) + " without the gmod-mcp signature");
    g_found_port = found;
    g_conflict = true;
    probe_next_ports(token);
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
        if (cancel->process_ended) {
            r.reached = true;
            r.aborted = true;
            return r;
        }
        cancel->cli = &cli;
    }
    // A refused connection right after the game answered is retried briefly (a burst of calls, a busy accept queue);
    // a closed game still fails within half a second.
    bool was_up = g_link == Link::Up;
    auto res = cli.Post("/mcp", h, payload, "application/json", receiver);
    for (int delay_ms : {100, 300}) {
        if (res || !was_up || res.error() != httplib::Error::Connection || decided || (cancel && (cancel->cancelled || cancel->process_ended))) break;
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
        if (cancel && cancel->process_ended) {  // issue #8: we cut it because the client it waited on died
            r.reached = true;
            r.aborted = true;
            return r;
        }
        auto e = res.error();
        r.refused = e == httplib::Error::Connection || e == httplib::Error::ConnectionTimeout;
        if (e == httplib::Error::Connection || e == httplib::Error::ConnectionTimeout)
            set_down("nothing listening: the game is closed or was started without -mcp");
        else
            set_down("connection error: " + httplib::to_string(e));
        return r;
    }
    r.reached = true;
    r.status = res->status;
    // Issue #5: who answered? gmod-mcp signs its answers (older modules: JSON body). Anything else is a foreign program.
    const bool ours = gmcp::is_gmod_mcp_reply(res->has_header(gmcp::kSignatureHeader), body);
    const bool stream = res->get_header_value("Content-Type").find("text/event-stream") != std::string::npos;
    if (!ours && !stream && (res->status >= 400 || !body.empty())) {
        r.reached = false;
        set_port_conflict(res->status, token);
        return r;
    }
    if (res->status == 401) {
        r.reached = false;
        set_down("the game rejected the token in " + g_dir + "/token");
        return r;
    }
    g_link = Link::Up;
    g_conflict = false;
    g_found_port = 0;
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
// The plugin option feedback_offers (Claude Code userConfig, given to the bridge as GMOD_MCP_FEEDBACK_OFFERS): on by
// default. On, the instructions' "Feedback:" paragraph (friction reports) gains how to turn it off; off, it is replaced.
bool feedback_offers_on() {
    const char *v = std::getenv("GMOD_MCP_FEEDBACK_OFFERS");
    if (!v) return true;
    std::string s = v;
    for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    return !(s == "false" || s == "0" || s == "no" || s == "off");
}

std::string patch_feedback(std::string text) {
    static const std::string head = "Feedback: while you work";
    size_t a = text.find(head);
    if (a == std::string::npos) return text;
    size_t b = text.find('\n', a);
    if (b == std::string::npos) b = text.size();
    if (feedback_offers_on()) {
        text.insert(b, " The user can turn these offers off: Claude Code plugin option feedback_offers (/plugin configure "
                       "gmod-mcp@gmod-mcp, or in a shell: echo '{\"feedback_offers\": \"false\"}' | claude plugin configure "
                       "gmod-mcp@gmod-mcp --values-stdin, then restart Claude Code). Say so in your offer, and set it for "
                       "them if they ask.");
    } else {
        text.replace(a, b - a, "Feedback: the user turned off friction-report offers (plugin option feedback_offers): do "
                               "not offer reports. They can turn them back on with /plugin configure gmod-mcp@gmod-mcp.");
    }
    return text;
}

json patch_capabilities(json result) {
    if (!result.is_object()) return result;
    if (result.contains("instructions") && result["instructions"].is_string())
        result["instructions"] = patch_feedback(result["instructions"].get<std::string>());
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
// --- Version of the module in the game. The plugin (this bridge, the skill, a copy of the module) is updated by
// Claude Code; the module loaded by the game is not. When they differ, every tool result says so, whatever the order in
// which Claude Code and the game started.
std::mutex g_ver_mu;
std::string g_module_version;  // "" = unknown (game not reached yet)

std::string version_warning() {
    std::lock_guard<std::mutex> lk(g_ver_mu);
    if (g_module_version.empty() || g_module_version == kBridgeVersion) return "";
    return std::string("WARNING: version mismatch. The gmod-mcp module running in Garry's Mod is ") + g_module_version +
           ", this Claude Code plugin is " + kBridgeVersion + ". Tools may behave differently from their descriptions or "
           "miss features: the tool list shown while the game was closed is this plugin's, the live one is the game "
           "module's, so tools listed earlier can be missing now. Tell the user now. To fix it: quit every game that runs "
           "gmod-mcp, then ask the gmod-mcp skill to update the module (it installs the module shipped with this plugin "
           "into the game).";
}

// Adds the warning to a tools/call result: first text item and structuredContent.version_warning.
void add_version_warning(json &result) {
    std::string w = version_warning();
    if (w.empty() || !result.is_object()) return;
    if (!result.contains("content") || !result["content"].is_array()) result["content"] = json::array();
    result["content"].insert(result["content"].begin(), json{{"type", "text"}, {"text", w}});
    if (result.contains("structuredContent") && result["structuredContent"].is_object()) result["structuredContent"]["version_warning"] = w;
}

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
struct EndSlot;
struct Launched {
    uint32_t pid = 0;
    std::string id;
    std::string owner;  // free label given at launch (a sub-agent's name): instances_close {owner} closes only those
    bool keep = false;
    std::string root;  // game folder it was started from (where its crash dumps go)
    std::shared_ptr<EndSlot> slot;  // how it ended, worked out once (shared by the copies of this record)
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

// --- Coordinator hand-over (issue #6) ---
// The game on the base port is the coordinator. When it ends while other games live, another takes the port over within a
// few seconds (module side); meanwhile nothing answers on the base port, then the new coordinator answers the retryable
// coordinator_changing for an instance that has not re-registered yet. The bridge rides both out for a few seconds, but
// only when other instances are known to live: with the only game closed a call must say game_not_running at once.
double g_handover_wait_s = 10.0;  // GMOD_MCP_HANDOVER_WAIT_S overrides it (tests)
std::atomic<int64_t> g_multi_seen_ms{0};

int64_t steady_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() + 1; }

// instances_list answer: several live instances seen now means a coordinator change is worth waiting for.
void note_instances(const json &sc) {
    int running = 0;
    for (const auto &e : sc.value("instances", json::array()))
        if (e.is_object() && e.value("state", "running") == "running") ++running;
    if (running >= 2) g_multi_seen_ms = steady_ms();
}

// A client this session launched, that answered once, still runs.
bool launched_client_alive() {
    std::lock_guard<std::mutex> lk(g_launch_mu);
    for (const auto &l : g_launched) {
        if (l.id.empty()) continue;
#ifdef _WIN32
        if (l.h && WaitForSingleObject(l.h, 0) == WAIT_TIMEOUT) return true;
#endif
    }
    return false;
}

bool handover_expected() {
    int64_t t = g_multi_seen_ms.load();
    return launched_client_alive() || (t != 0 && steady_ms() - t < 30000);
}

// The result of a tool call is the retryable coordinator_changing: *retry_after gets the advised delay.
bool coordinator_changing_result(const json &result, double *retry_after) {
    if (!result.is_object()) return false;
    const json &sc = result.contains("structuredContent") ? result["structuredContent"] : json();
    if (!sc.is_object() || !sc.contains("error") || !sc["error"].is_object() || sc["error"].value("code", "") != "coordinator_changing") return false;
    const json &d = sc["error"].contains("details") ? sc["error"]["details"] : json();
    if (retry_after) {
        *retry_after = d.is_object() && d.contains("retry_after_s") && d["retry_after_s"].is_number() ? d["retry_after_s"].get<double>() : 1.0;
    }
    return true;
}

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
         "it keeps their exit short; a clean quit is safe too, see the convar note below). Sub-agents of one Claude Code session share "
         "this bridge: give each one's clients an `owner` label and close them with instances_close {owner}, never "
         "{all: true}, which would close the other sub-agents' clients too. The game is found through Steam; pass "
         "game_dir (the folder holding gmod_win64.exe or bin/win64/gmod.exe) if it is elsewhere. Windows only for now.\n"
         "Presets: 'light' (default: 640x360, 20 fps; for most tests, NOT for performance measurements: the 20 fps cap "
         "hides per-frame cost) and 'normal' (640x360, 30 fps; for rendering checks, screenshots and frame-time sensitive "
         "tests). sound: false (-nosound) changes what the engine reports about sounds: SoundDuration of compressed "
         "(ADPCM) WAV files differs (0.49 s instead of 1.24 s seen); keep sound on for sound tests.\n"
         // Issue #10
         "Pass-through options (checked before anything starts; a refused one is invalid_params naming it): fps_max and "
         "fps_max_nofocus (integers: 0 = no cap, or 10 to 1000) replace the preset's frame cap (fps_max_nofocus follows "
         "fps_max when only that one is given): timing-sensitive tests give different verdicts at 20, 45 and 300 fps, and "
         "perf measurements need more than 30. commands: console commands run on each client once it answers, before `map` "
         "loads, one at a time in order, e.g. [\"fps_max 300\", \"snd_mute_losefocus 0\"] (a `;` splits a line as in the console); "
         "refused: quit and exit (close with instances_close), connect, retry, redirect and connect_lobby (gmod-mcp never "
         "connects to a server) and map changes (map, changelevel, load: use `map`). Each result is in "
         "instances[i].commands [{cmd, ok, error?}]; a command that fails makes the call isError, the others still run. "
         "launch_args: extra engine command-line tokens, one per array element (a printable ASCII word without spaces, quotes "
         "or `;`), e.g. [\"-noborder\", \"+cl_showfps\", \"1\"]; a `+name value` token runs a console command at startup. "
         "Refused: what gmod-mcp sets itself (-multirun, -mcp*, -w, -h, -width, -height and the window-mode options, "
         "-dxlevel, -condebug, -game), +connect and the other connecting or map-loading commands. -nosound is the same as "
         "sound: false. Archived convars changed this way (fps_max, a `+convar` token, a command such as `cl_showfps 1` or "
         "`toggle snd_mute_losefocus`) are given back to the user's value before a clean quit like the frame cap (needs the "
         "module of this release in the game folder: an older module restores only fps_max, fps_max_nofocus, "
         "engine_no_focus_sleep and snd_mute_losefocus; a convar changed by `exec` of a cfg file is not tracked). "
         "Where the options are listed: command-line options, Valve Developer Community \"Command line options\" "
         "(https://developer.valvesoftware.com/wiki/Command_line_options) and, for the GMod-specific ones, the Garry's Mod "
         "wiki (wiki_search \"command line\"); console commands and convars: api_search and concommand_list on a running "
         "client.\n"
         "All clients share the game folder with the user's own game: same addons, cfg, data/ and cookies (sv.db/cl.db). "
         "A test that writes cookies, data files or archived convars changes the user's game too: clean up after it. "
         "The frame cap and the focus options these clients start with (fps_max, fps_max_nofocus, snd_mute_losefocus) are "
         "archived convars: the module gives each one the user's value from cfg/config.cfg (its default when the file has "
         "no line) before the client quits cleanly (game_quit, console quit, the Quit button, the window's close button; "
         "not a `quit` the user types by hand in that client's console), so none of them leaks into the user's config. "
         "Closing with instances_close kills the process and writes nothing.\n"
         "Video settings: the clients start with -dxlevel 95, so the engine neither reads nor writes the user's stored video "
         "mode (resolution, anti-aliasing): the user's own game keeps its resolution.\n"
         "Measured cost per client on gm_construct (16-core PC, the game and its 4 browser subprocesses): light about 19% "
         "of one core and 1.5 GB of RAM; normal 24% and 1.5 GB. Eight clients: about 1.5 cores and 12 GB. Memory is the "
         "limit, not CPU. The frame cap is what saves CPU (30 to 20 fps: -20%); sound: false saves about 12% more. Window "
         "size barely matters, and a client narrower than 640 pixels crashes while loading a map (integer division by zero "
         "in client.dll at client Lua startup: seen at 480x270 and 320x240; it only survived with the HUD turned off). "
         "-textmode and lower graphics settings were measured: no real gain (and graphics settings would be saved into the "
         "user's own), so neither is offered.\n"
         "Result: {instances: [{instance, pid, port, owner?, commands? [{cmd, ok, error?}], map? (or map: null + map_error)}], game_dir, found_by, preset, "
         "keep, fps_max, fps_max_nofocus, fps_note, launch_args?, lua_mount}. If they do not all answer within timeout_s: error timeout with silent_pids "
         "(started but never answered: close them with instances_close {pids}) and answered [{instance, pid}]. A client that "
         "crashed during startup or its map load is reported as client_crashed (startup: the whole call's error; map load: that "
         "client's map_error), with the minidump file name in details.dump (for a human) and exit_code: launch a replacement. Example: "
         "{\"count\": 2, \"map\": \"gm_construct\", \"owner\": \"fork-a\"}, then pass each returned instance id as "
         "`instance`.\n"
         // FP7137 (git grep FP7137: agent-facing text A1-A3, see docs/FP7137.md)
         "Starting a game makes every other running Garry's Mod release its GPU resources for 2-10 s, and a map load that "
         "ends in that window crashes that game (engine bug, Facepunch garrysmod-issues #7137). So the bridge serialises "
         "the launches and map loads that go through it, from any session: this call first waits while any game loads a "
         "map, and with `map` the maps load after a 15 s settle delay, 2 at a time. \"Any game\" includes loads the bridge did "
         "not start (console `map`, changelevel, Lua RunConsoleCommand, a script talking to the game): every game reports "
         "when it is loading a map (instances_list `loading`), and waited_reason then starts with map_load_outside_bridge. "
         "A call that waited reports waited_s and "
         "waited_reason (and progress when your client sent a progress token). If the wait would exceed timeout_s it fails "
         "with error map_load_in_progress and starts nothing: retry later, or with a larger timeout_s. Not covered: a game "
         "started by hand (not by this call) and a load that begins while a launch is already creating its processes "
         "(only launches and map_load calls of the bridge wait, a load started by other means is never held back).\n"
         "Right after a game starts, the other games can be unreachable for 15-18 s (instance_unreachable or timeouts): "
         "expected, wait and retry; it is not a dead game (client_crashed is the signal for a dead one).\n"
         "Order: launch all the clients you need in ONE call (with `map`, they load 2 at a time after the settle delay), "
         "then work. Do not interleave launches with map loads, yours or another session's."},
        {"inputSchema",
         {{"type", "object"},
          {"properties",
           {{"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 8}, {"description", "How many clients (default 1)."}}},
            {"map", {{"type", "string"}, {"pattern", "^[A-Za-z0-9_.-]{1,64}$"}, {"description", "Map to load on each, e.g. gm_construct. Default: stay at the main menu."}}},
            {"maxplayers", {{"type", "integer"}, {"minimum", 1}, {"maximum", 128}, {"description", "For map_load (default 1: singleplayer)."}}},
            {"preset", {{"type", "string"}, {"enum", json::array({"light", "normal"})}, {"description", "Default light."}}},
            {"sound", {{"type", "boolean"}, {"description", "Keep sound on (default true). false saves CPU when sound does not matter."}}},
            {"fps_max", {{"type", "integer"}, {"minimum", 0}, {"maximum", 1000},  // Issue #10
                         {"description", "Frame cap of these clients (fps_max): 0 = no cap, or 10 to 1000. Default: the preset's (light 20, normal 30). Given back to the user's value on a clean quit."}}},
            {"fps_max_nofocus", {{"type", "integer"}, {"minimum", 0}, {"maximum", 1000},  // Issue #10
                                 {"description", "Frame cap when the window has no focus (fps_max_nofocus): 0 = no cap, or 10 to 1000. Default: the same as fps_max."}}},
            {"commands", {{"type", "array"}, {"maxItems", 32}, {"items", {{"type", "string"}, {"maxLength", 256}}},  // Issue #10
                          {"description", "Console commands run on each client once it answers, before `map` loads, e.g. [\"fps_max 300\", \"snd_mute_losefocus 0\"]. quit, exit, connect and map changes are refused. Archived convars they change are given back on a clean quit."}}},
            {"launch_args", {{"type", "array"}, {"maxItems", 32}, {"items", {{"type", "string"}, {"maxLength", 160}}},  // Issue #10
                             {"description", "Extra engine command-line tokens, one per element, e.g. [\"-noborder\", \"+cl_showfps\", \"1\"]. Refused: what gmod-mcp sets itself (-multirun, -mcp*, -w, -h, window mode, -dxlevel, -condebug, -game), +connect, +map. Options: https://developer.valvesoftware.com/wiki/Command_line_options"}}},
            {"keep", {{"type", "boolean"}, {"description", "Keep them running after this session ends (default false)."}}},
            {"owner", {{"type", "string"}, {"maxLength", 64}, {"description", "Label recorded with these clients (e.g. your sub-agent's name or task); instances_close {owner} then closes only them."}}},
            {"lua_mount", {{"type", "string"}, {"maxLength", 1024},
                         {"description", "Absolute folder whose Lua these clients load before every addon's (e.g. your git worktree of "
                                         "the addon: its root or its lua/ folder). Applies from their first map load; see lua_mount. "
                                         "Each engine is asked what it really has BEFORE any map loads (instances[i].lua_mount.active); a missing mount is "
                                         "applied again, and a client that still has none gets no map (map_error lua_mount_not_active, "
                                         "isError, warning). Windows only."}}},
            {"game_dir", {{"type", "string"}, {"description", "Game folder (holding gmod_win64.exe or bin/win64/gmod.exe), if Steam does not know it."}}},
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
         "game_terminate. Sub-agents of one Claude Code session share it: all: true closes their clients too. The selectors "
         "add up (instances, pids, owner). Result: {closed: [{pid, instance?, owner?}], refused?: [ids, pids or "
         "\"owner:<label>\" not started by this session or already closed], why_refused?}; an error only when nothing was "
         "closed. Closing the client that is the coordinator (the game on the base port, see instances_list: role, "
         "coordinator_warning) while other games live moves that role: the answer says so (closed[i].was_coordinator, "
         "coordinator_moved_to, warning) and calls of other agents answer coordinator_changing for a few seconds. "
         "Example: {\"owner\": \"fork-a\"}."},
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
         "calls too (they would act on its game). Pass `instance` on every call instead. Result: {default_instance, previous}. The id is not checked: an unknown one makes later calls fail with unknown_instance."},
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

// How a client this session launched ended: this bridge holds its process handle, so the exit code and end time are known
// exactly, whatever the coordinator saw. Worked out once (a dump is given to one process only).
struct EndSlot {
    std::mutex mu;
    bool done = false;
    crashinfo::Verdict verdict;
    uint32_t exit_code = 0;
    double ended_unix = 0;
};
std::mutex g_claimed_mu;
std::set<std::string> g_claimed_dumps;

// True when the launched client's process has exited (then `v`, `code` and `ended` are filled).
// `wait_ms`: how long to wait for a process that has not ended yet. A crash closes the game's sockets before the process object is
// signalled, so a call that just failed on a broken connection asks again after a moment (issue #8).
bool launched_end(const Launched &l, crashinfo::Verdict &v, uint32_t &code, double &ended, int wait_ms = 0) {
#ifdef _WIN32
    if (!l.slot) return false;
    std::lock_guard<std::mutex> lk(l.slot->mu);
    if (!l.slot->done) {
        plat::ProcInfo pi;
        if (!l.h || !plat::process_watch_query((void *)l.h, pi, wait_ms) || !pi.exited) return false;
        crashinfo::Facts f;
        f.exit_known = true;
        f.exit_code = pi.exit_code;
        f.exit_unix = pi.exit_unix;
        f.start_unix = pi.start_unix;
        f.last_seen_unix = pi.exit_unix;
        f.now_unix = pi.exit_unix;
        std::vector<crashinfo::Dump> dumps = crashinfo::list_dumps(crashinfo::dump_dirs(l.root + "/garrysmod"));
        std::lock_guard<std::mutex> ck(g_claimed_mu);
        l.slot->verdict = crashinfo::judge(f, dumps, g_claimed_dumps);
        if (!l.slot->verdict.dump.empty()) g_claimed_dumps.insert(l.slot->verdict.dump);
        l.slot->exit_code = pi.exit_code;
        l.slot->ended_unix = pi.exit_unix;
        l.slot->done = true;
    }
    v = l.slot->verdict;
    code = l.slot->exit_code;
    ended = l.slot->ended_unix;
    return true;
#else
    (void)l; (void)v; (void)code; (void)ended; (void)wait_ms;
    return false;
#endif
}

double unix_now() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

// The error object for a launched client that crashed ({code, message, details}), or null when it still runs or ended normally.
json launched_crash_error(const Launched &l, int wait_ms = 0) {
    crashinfo::Verdict v;
    uint32_t code = 0;
    double ended = 0;
    if (!launched_end(l, v, code, ended, wait_ms) || v.cause != crashinfo::Cause::Crashed) return json();
    double ago = std::max(0.0, unix_now() - ended);
    json details{{"instance", l.id.empty() ? json(nullptr) : json(l.id)}, {"pid", l.pid}, {"dump", v.dump.empty() ? json(nullptr) : json(v.dump)},
                 {"evidence", v.evidence}, {"exit_code", code}};
    return json{{"code", crashinfo::crash_code("client")},
                {"message", crashinfo::crash_message("client", l.id.empty() ? "started by this session" : l.id, l.pid, ago, v, true, code)},
                {"details", details}};
}

// instances_list, completed by the bridge: the owner label of each client it launched, the clients it launched that
// never answered (gmod-mcp not loaded, still starting), and an optional owner filter. `sc` is the game's answer
// (or an empty list when no game answers).
json annotate_instances(json sc, const json &args) {
    std::string want = args.value("owner", "");
    std::set<uint32_t> listed;
    json kept = json::array();
    note_instances(sc);
    json coord_warning;  // issue #6: the coordinator is a client this session launched, and other instances depend on it
    {
        int running = 0;
        for (const auto &e : sc.value("instances", json::array()))
            if (e.is_object() && e.value("state", "running") == "running") ++running;
        std::lock_guard<std::mutex> lk(g_launch_mu);
        for (const auto &e : sc.value("instances", json::array())) {
            if (!e.is_object() || e.value("role", "") != "coordinator" || running < 2) continue;
            for (const auto &l : g_launched)
                if (l.pid == e.value("pid", 0u)) {
                    coord_warning = e.value("id", "?") + " is the coordinator (it holds the directory of instances and relays the calls to the others) and " +
                                    (l.owner.empty() ? "was launched by this session" : "was launched with owner \"" + l.owner + "\"") +
                                    ": closing it (instances_close, game_quit) moves the role to another instance, and calls with `instance` answer "
                                    "coordinator_changing for a few seconds. Close the other clients first, or expect the pause.";
                }
        }
    }
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
        if (l.h && WaitForSingleObject(l.h, 0) == WAIT_OBJECT_0) {  // exited: shown when it crashed (a clean quit just goes)
            crashinfo::Verdict v;
            uint32_t code = 0;
            double ended = 0;
            if (launched_end(l, v, code, ended) && v.cause == crashinfo::Cause::Crashed) {
                json e = crashinfo::crashed_entry(l.id.empty() ? "pid-" + std::to_string(l.pid) : l.id, "client", l.pid,
                                                  std::max(0.0, unix_now() - ended), v, true, code);
                e["launched_by_this_session"] = true;
                if (!l.owner.empty()) e["owner"] = l.owner;
                sc["instances"].push_back(e);
            }
            continue;
        }
#endif
        json x{{"pid", l.pid}, {"note", "launched by this session, not answering: still starting, or gmod-mcp did not load in it "
                                        "(see garrysmod/data/mcpagent/last_crash.txt). instances_close {pids} closes it."}};
        if (!l.owner.empty()) x["owner"] = l.owner;
        silent.push_back(x);
    }
    if (!silent.empty()) sc["launched_not_answering"] = silent;
    if (!want.empty()) sc["owner_filter"] = want;
    if (coord_warning.is_string()) sc["coordinator_warning"] = coord_warning;
    return sc;
}

json tool_text(json structured, bool error) {
    std::string t = structured.dump(-1, ' ', false, json::error_handler_t::replace);
    return json{{"content", json::array({json{{"type", "text"}, {"text", t}}})}, {"structuredContent", std::move(structured)}, {"isError", error}};
}
json tool_error(const std::string &code, const std::string &message) {
    return tool_text(json{{"error", {{"code", code}, {"message", message}}}}, true);
}

// Calls `again` while the coordinator role moves (see the hand-over notes above), for at most g_handover_wait_s: nothing
// listening on the base port while other instances live, or an answer that is coordinator_changing (nothing was
// executed in either case, so repeating the call is safe). If nothing came back by then, an unreachable game becomes the
// retryable coordinator_changing instead of game_not_running; an answer that is still coordinator_changing is returned as is.
Relay ride_out_handover(Relay r, const std::function<Relay()> &again, const std::atomic<bool> *cancelled, const json &id) {
    auto t0 = std::chrono::steady_clock::now();
    auto waited = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    for (;;) {
        double retry = 1.0;
        bool wait = false;
        if (!r.reached) wait = r.refused && handover_expected();
        else if (r.response.is_object() && r.response.contains("result")) wait = coordinator_changing_result(r.response["result"], &retry);
        if (!wait) return r;
        if (waited() >= g_handover_wait_s || (cancelled && cancelled->load())) {
            if (!r.reached) {
                json e{{"error", {{"code", "coordinator_changing"},
                                  {"message", "the coordinator (the game on port " + std::to_string(g_port) + ") stopped answering while other instances "
                                              "live, and none has taken the role over yet (waited " + std::to_string((int)waited()) + " s). Retry in a few seconds; "
                                              "nothing was sent to the game."},
                                  {"details", {{"retry_after_s", 3.0}, {"waited_s", std::round(waited() * 10) / 10}}}}}};
                r.reached = true;
                r.status = 200;
                r.response = rpc_result(id, tool_text(e, true));
            }
            return r;
        }
        double nap = std::min(std::max(retry, 0.3), 1.0);
        for (double t = 0; t < nap && !(cancelled && cancelled->load()); t += 0.05) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        r = again();
    }
}

// A tool of the game, called by the bridge itself (legacy era, no stream). Returns its structuredContent, or
// {"error": ...} when the game is unreachable or the call failed.
json game_call(const std::string &tool, json args, double timeout_s) {
    json m{{"jsonrpc", "2.0"}, {"id", "gmod-mcp-bridge-internal"}, {"method", "tools/call"},
           {"params", {{"name", tool}, {"arguments", std::move(args)}}}};
    Relay r = ride_out_handover(relay(m, timeout_s), [&] { return relay(m, timeout_s); }, nullptr, m["id"]);
    if (!r.reached) return json{{"error", {{"code", unreachable_code()}, {"message", g_conflict ? port_conflict_text() : why_down()}}}};
    if (!r.response.contains("result")) return json{{"error", {{"code", "rpc_error"}, {"message", r.response.value("error", json::object()).value("message", "no result")}}}};
    return r.response["result"].value("structuredContent", json::object());
}

std::string parent_of(std::string p) {
    while (!p.empty() && (p.back() == '/' || p.back() == '\\')) p.pop_back();
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(0, s);
}

// The game folder: the argument, then what the module reported (status: game_dir), then Steam's libraries.
[[maybe_unused]] std::string find_game_root(const std::string &arg, std::string &how) {
    auto ok = [](std::string d) {
        while (!d.empty() && (d.back() == '/' || d.back() == '\\')) d.pop_back();
        std::string low = d;
        for (auto &c : low) c = (char)std::tolower((unsigned char)c);
        if (low.size() >= 9 && low.compare(low.size() - 9, 9, "garrysmod") == 0 && gamedir::find_game_exe(d).empty()) d = parent_of(d);
        return gamedir::find_game_exe(d).empty() ? std::string() : d;
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

thread_local double g_fp7137_progress_offset = 0;  // FP7137: progress already reported while waiting for the lock

[[maybe_unused]] void progress(const json &token, double done, double total, const std::string &message) {
    if (token.is_null()) return;
    json p{{"progressToken", token}, {"progress", done + g_fp7137_progress_offset}, {"message", message}};  // FP7137
    if (total > 0) p["total"] = total + g_fp7137_progress_offset;  // FP7137
    out(json{{"jsonrpc", "2.0"}, {"method", "notifications/progress"}, {"params", p}});
}

#ifdef _WIN32
// UTF-8 to UTF-16: paths and the command line arrive as UTF-8 (JSON); widening byte by byte corrupts every non-ASCII
// character.
std::wstring utf8_to_wide(const std::string &s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring(s.begin(), s.end());
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
#endif

// Pure: what one launched client's engine has for the requested Lua mount, from what the game's own `lua_mount` tool
// answered (its structuredContent, or {"error"} when the call failed). The mount counts as active only when the
// engine reports the requested folder; the answer's base_lua_mounted false means the client cannot run its base Lua.
[[maybe_unused]] json lua_mount_verdict(const std::string &requested, const json &answer) {
    auto same = [](std::string a, std::string b) {
        auto norm = [](std::string &x) {
            for (auto &c : x) {
                if (c == '/') c = '\\';
                else if ((unsigned char)c < 0x80) c = (char)std::tolower((unsigned char)c);
            }
            while (x.size() > 3 && x.back() == '\\') x.pop_back();
        };
        norm(a);
        norm(b);
        return !a.empty() && a == b;
    };
    json v{{"requested", requested}, {"active", false}, {"base_lua_mounted", nullptr}};
    if (!answer.is_object() || answer.contains("error")) {
        json e = answer.is_object() && answer.contains("error") ? answer["error"] : json("no usable answer from the game");
        json m = e.is_object() && e.contains("message") ? e["message"] : e;
        v["error"] = m.is_string() ? m.get<std::string>() : m.dump();
        return v;
    }
    if (answer.contains("base_lua_mounted") && answer["base_lua_mounted"].is_boolean()) v["base_lua_mounted"] = answer["base_lua_mounted"];
    json engine = answer.value("lua_mount", json());
    if (engine.is_string() && same(engine.get<std::string>(), requested)) {
        v["active"] = true;
    } else {
        v["engine_lua_mount"] = engine.is_string() ? engine : json(nullptr);
        v["error"] = engine.is_string() ? "the engine has another folder mounted: " + engine.get<std::string>()
                                        : "the engine has no mounted folder: the module did not apply -mcp_lua_mount "
                                          "(see garrysmod/console.log); the tests would run the installed addon's code";
    }
    return v;
}

// FP7137 (issue #7): ids of the games that report a map load in progress (the `loading` flag each module sends with its
// heartbeat), whatever started the load: the console, Lua, a script talking to the game, a bridge. `except` is skipped.
std::vector<std::string> fp7137_loading_games(const std::string &except = "") {  // FP7137
    std::vector<std::string> ids;  // FP7137
    json lst = game_call("instances_list", json::object(), 3);  // FP7137: an unreachable game is an error object: no ids
    if (!lst.is_object() || !lst.contains("instances") || !lst["instances"].is_array()) return ids;  // FP7137
    for (const auto &in : lst["instances"])  // FP7137
        if (in.is_object() && in.value("loading", false) && in.value("state", "running") == "running" && in.value("id", "") != except)  // FP7137
            ids.push_back(in.value("id", "?"));  // FP7137
    return ids;  // FP7137
}  // FP7137

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
    // Issue #10: pass-through options, checked before anything starts (the schema already checked the types and bounds).
    int fps_cap = preset == "normal" ? 30 : 20, fps_cap_nofocus = fps_cap;
    bool fps_custom = args.contains("fps_max") || args.contains("fps_max_nofocus");
    for (const char *key : {"fps_max", "fps_max_nofocus"}) {
        if (!args.contains(key)) continue;
        std::string fe = launchopts::fps_error(args[key], key);
        if (!fe.empty()) return tool_error("invalid_params", fe);
    }
    if (args.contains("fps_max")) fps_cap = fps_cap_nofocus = (int)std::lround(args["fps_max"].get<double>());  // the unfocused cap follows...
    if (args.contains("fps_max_nofocus")) fps_cap_nofocus = (int)std::lround(args["fps_max_nofocus"].get<double>());  // ...unless given
    std::vector<std::string> startup_cmds, extra_args;
    if (args.contains("commands")) {
        std::string ce = launchopts::check_commands(args["commands"], startup_cmds);
        if (!ce.empty()) return tool_error("invalid_params", ce);
    }
    if (args.contains("launch_args")) {
        std::string ae = launchopts::check_launch_args(args["launch_args"], extra_args);
        if (!ae.empty()) return tool_error("invalid_params", ae);
    }
    // Checked before anything starts: a bad folder must not leave clients behind.
    std::string mount_root;
    if (args.contains("lua_mount") && args["lua_mount"].is_string() && !args["lua_mount"].get<std::string>().empty()) {
        std::string oerr;
        if (!lua_mount::normalize_root(args["lua_mount"].get<std::string>(), mount_root, oerr)) return tool_error("invalid_params", "lua_mount: " + oerr);
    }
    std::string how, root = find_game_root(args.value("game_dir", ""), how);
    if (root.empty()) {
        // Issue #4: the 64-bit game is gmod_win64.exe at the root (older installs) or bin/win64/gmod.exe (current ones).
        std::string given = args.value("game_dir", "");
        return tool_error("game_not_found", "Garry's Mod not found (" + how + "): pass game_dir, the folder holding gmod_win64.exe or bin/win64/gmod.exe (the gmod-mcp skill explains how to find it)" +
                                                (given.empty() ? "" : "; " + gamedir::missing_exe_message(given)));
    }
    if (!mount_root.empty()) {
        std::string inside = lua_mount::refuse_inside_game(mount_root, root + "/garrysmod");
        if (!inside.empty()) return tool_error("invalid_params", "lua_mount: " + inside);
    }
    // Fixed option set (no free arguments). The light preset comes from measurements of what actually saves CPU and
    // memory per client.
    // -mcp_test_client marks the game as one of ours: the module gives the archived convars below back to the user's
    // values before a clean quit (they would otherwise be saved into the config.cfg the user's own game shares).
    std::string opts = "-multirun -mcp -mcp_test_client -windowed -novid +engine_no_focus_sleep 0 +snd_mute_losefocus 0";
    // Issue #10: the preset's frame cap, or the one requested. Narrower than 640 crashes the client.
    opts += " -w 640 -h 360 +fps_max " + std::to_string(fps_cap) + " +fps_max_nofocus " + std::to_string(fps_cap_nofocus);
    // The engine writes the video mode (HKCU\Software\Valve\Source\garrysmod\Settings: ScreenWidth, ScreenHeight,
    // ScreenMSAA, ...) at startup and at exit, which would overwrite the user's own resolution with 640x360. Both
    // writes are skipped when -dxlevel is on the command line (engine.dll, read-only disassembly 2026-11-15), and with
    // it the stored settings are not read either. 95 is the level the engine itself keeps on current hardware
    // (DXLevel_V1 = 0x5f), so rendering is unchanged. No graphics option other than that one is added (mat_* convars
    // are archived into cfg/config.cfg, shared with the user's game, for little gain).
    opts += " -dxlevel 95";
    if (!sound) opts += " -nosound";
    // The folder was checked: no quote or line break can end the argument.
    if (!mount_root.empty()) opts += " -mcp_lua_mount \"" + mount_root + "\"";
    // Issue #10: the extra tokens (validated: no space, quote or ';'), after ours so that a `+convar` of theirs wins; then the
    // names of the convars they and the commands may change, which the module gives back to the user's values before a
    // clean quit (an older module ignores this option).
    for (const auto &t : extra_args) opts += " " + t;
    std::vector<std::string> restore = launchopts::restore_names(startup_cmds, extra_args);
    if (!restore.empty()) opts += " -mcp_restore_convars " + launchopts::join_list(restore);
    SetEnvironmentVariableA("SteamAppId", "4000");
    SetEnvironmentVariableA("SteamGameId", "4000");
    std::string exe = gamedir::find_game_exe(root);  // Issue #4: gmod_win64.exe or bin/win64/gmod.exe
    for (auto &c : exe)
        if (c == '/') c = '\\';
    // FP7137: wait while a map loads in any game (shared lock), then hold the launch slot while the processes are created.
    fp7137::Gate fp_gate;  // FP7137
    fp_gate.enter(fp7137::Kind::Launch, "launch", timeout, 60, nullptr, [&](double w, const std::string &why) { progress(token, w, 0, why); }, [] { return fp7137_loading_games(); });  // FP7137 (issue #7: also waits for loads no bridge saw)
    if (!fp_gate.ok()) return fp_gate.error_result();  // FP7137
    if (fp_gate.waited_s() > 0) g_fp7137_progress_offset = fp_gate.waited_s() + 1;  // FP7137
    std::vector<Launched> started;
    for (int i = 0; i < count; ++i) {
        std::string cmd = "\"" + exe + "\" " + opts;
        std::wstring wcmd = utf8_to_wide(cmd), wdir = utf8_to_wide(root);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE, 0, nullptr, wdir.c_str(), &si, &pi)) {
            for (auto &l : started) TerminateProcess(l.h, 0);
            return tool_error("launch_failed", "CreateProcess failed (error " + std::to_string(GetLastError()) + ") for " + exe);
        }
        CloseHandle(pi.hThread);
        fp_gate.note_started();  // FP7137
        Launched l;
        l.pid = pi.dwProcessId;
        l.h = pi.hProcess;
        l.keep = keep;
        l.owner = owner;
        l.root = root;
        l.slot = std::make_shared<EndSlot>();
        started.push_back(l);
    }
    fp_gate.release();  // FP7137: the settle delay counts from the last start, the slot is no longer needed
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
        note_instances(lst);
        for (const auto &in : lst.value("instances", json::array()))
            for (auto &l : started)
                if (in.value("pid", 0u) == l.pid && in.value("state", "") != "crashed" && !found.contains(std::to_string(l.pid))) {
                    found[std::to_string(l.pid)] = in;
                    l.id = in.value("id", "");
                    progress(token, (double)found.size(), count, l.id + " answers");
                }
        for (auto &l : started)
            if (WaitForSingleObject(l.h, 0) == WAIT_OBJECT_0 && !found.contains(std::to_string(l.pid))) {
                json ce = launched_crash_error(l);
                if (!ce.is_null()) return tool_text(json{{"error", ce}}, true);
                return tool_error("launch_failed", "client pid " + std::to_string(l.pid) + " exited during startup (Steam not running? see garrysmod/console.log)");
            }
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
                              {"lua_mount", mount_root.empty() ? json(nullptr) : json(mount_root)},
                              {"lua_mount_note", mount_root.empty() ? json(nullptr)
                                                                    : json("the clients that did not answer keep running with -mcp_lua_mount; none was checked or loaded a map. Once one answers "
                                                                           "(instances_list), check it with lua_mount {instance} before map_load, or close it (instances_close {pids})")}},
                         true);
    }
    // The requested mount is only a request: ask each engine what it really has (the module logs and ignores a folder it
    // cannot apply) BEFORE any map loads, and apply it again where it is missing. A client that still has none gets no map
    // (a test must never run on the installed addon's code without the agent knowing: issue #6).
    std::vector<json> mount_res(started.size());
    std::vector<char> mount_ok(started.size(), 1);
    json mount_problems = json::array();
    if (!mount_root.empty()) {
        progress(token, 0, count, "checking the Lua mount of each client before the map loads");
        std::vector<std::thread> th;
        for (size_t i = 0; i < started.size(); ++i)
            th.emplace_back([&, i] {
                const std::string iid = started[i].id;
                json v = lua_mount_verdict(mount_root, game_call("lua_mount", json{{"instance", iid}}, 20));
                if (!v.value("active", false)) {
                    json ap = game_call("lua_mount", json{{"instance", iid}, {"path", mount_root}}, 30);
                    json v2 = lua_mount_verdict(mount_root, ap);
                    if (v2.value("active", false)) v2["reapplied"] = true;
                    else if (v.contains("error")) v2["first_check_error"] = v["error"];
                    v = v2;
                }
                mount_res[i] = v;
            });
        for (auto &t : th) t.join();
        for (size_t i = 0; i < started.size(); ++i) {
            const json &v = mount_res[i];
            mount_ok[i] = v.value("active", false);
            if (!v.value("active", false)) mount_problems.push_back(started[i].id + ": " + v.value("error", std::string("not active")));
        }
    }
    // Issue #10: the startup commands, on every client, before any map loads. One thread per client, the commands of a
    // client in order; a failing command is reported and the next ones still run.
    std::vector<json> cmd_res(started.size(), json::array());
    bool commands_ok = true;
    if (!startup_cmds.empty()) {
        progress(token, count, count, "running " + std::to_string(startup_cmds.size()) + " startup command(s) on each client");
        std::vector<std::thread> th;
        for (size_t i = 0; i < started.size(); ++i)
            th.emplace_back([&, i] {
                for (const auto &c : startup_cmds) {
                    json r = game_call("console_exec", json{{"cmd", c}, {"instance", started[i].id}, {"wait_frames", 2}}, 30);
                    json e{{"cmd", c}, {"ok", !r.contains("error")}};
                    if (r.contains("error")) e["error"] = r["error"];
                    cmd_res[i].push_back(e);
                }
            });
        for (auto &t : th) t.join();
        for (const auto &arr : cmd_res)
            for (const auto &e : arr) commands_ok = commands_ok && e.value("ok", false);
    }
    // Maps: at most 2 at a time, after the settle delay (FP7137: was all at once).
    json results = json::array();
    std::vector<json> map_res(started.size());
    if (!map.empty()) {
        progress(token, 0, count, "loading " + map);
        fp7137::for_each_limited(started.size(), [&](size_t i) {  // FP7137: at most 2 at a time (was one thread per client, all at once)
                if (!mount_ok[i]) {
                    map_res[i] = json{{"error", {{"code", "lua_mount_not_active"},
                                                 {"message", "map " + map + " not loaded: the Lua mount " + mount_root + " is not active on this client (" +
                                                                 mount_res[i].value("error", std::string("unknown")) + "), the tests would run the installed addon's code"}}}};
                    return;
                }
                fp7137::Gate fp_map;  // FP7137: after the settle delay, never while another launch starts a game
                fp_map.enter(fp7137::Kind::MapLoad, "map:" + map, std::max(30.0, timeout - elapsed()), timeout + 10, nullptr, nullptr);  // FP7137
                if (!fp_map.ok()) {  // FP7137
                    map_res[i] = fp_map.error_result()["structuredContent"];  // FP7137
                    return;  // FP7137
                }
                json a{{"map", map}, {"instance", started[i].id}, {"timeout_s", std::max(30.0, timeout - elapsed())}};
                if (args.contains("maxplayers")) a["maxplayers"] = args["maxplayers"];
                map_res[i] = game_call("map_load", a, std::max(40.0, timeout - elapsed() + 10));
                if (fp_map.waited_s() >= 0.05) map_res[i]["fp7137_waited"] = json{{"waited_s", std::round(fp_map.waited_s() * 10) / 10}, {"waited_reason", fp_map.reason()}};  // FP7137
        });  // FP7137
        // A client that died during its load: say it crashed (the game's answer is only that nothing answers).
        // Issue #8: when the answer says nothing answers (the connection broke), the process object may be signalled a moment
        // later than the socket closes: wait for it before concluding the client lives.
        for (size_t i = 0; i < started.size(); ++i)
            if (map_res[i].contains("error") && mount_ok[i]) {
                std::string code = map_res[i]["error"].is_object() ? map_res[i]["error"].value("code", "") : "";
                bool broken = code == "game_not_running" || code == "port_conflict" || code == "instance_gone" || code == "instance_unreachable" ||
                              code == "coordinator_changing" || code == "relay_error" || code == "rpc_error";
                json ce = launched_crash_error(started[i], broken ? 2500 : 0);
                if (!ce.is_null()) map_res[i] = json{{"error", ce}};
            }
        // With the map loaded the engine's server Lua paths exist: look again, for the base Lua (a mounted folder that hides
        // the game's own lua/ makes a client unusable).
        std::vector<std::thread> th;
        for (size_t i = 0; i < started.size(); ++i)
            if (!mount_root.empty() && mount_ok[i] && !map_res[i].contains("error"))
                th.emplace_back([&, i] {
                    json v = lua_mount_verdict(mount_root, game_call("lua_mount", json{{"instance", started[i].id}}, 20));
                    if (mount_res[i].value("reapplied", false)) v["reapplied"] = true;
                    mount_res[i] = v;
                });
        for (auto &t : th) t.join();
    }
    if (!mount_root.empty())
        for (size_t i = 0; i < started.size(); ++i) {
            const json &v = mount_res[i];
            if (mount_ok[i] && !v.value("active", false)) mount_problems.push_back(started[i].id + ": " + v.value("error", std::string("not active after the map load")));
            else if (v.value("base_lua_mounted", json()) == json(false))
                mount_problems.push_back(started[i].id + ": the game's own lua/ folder is missing from its server Lua paths (base Lua not mounted)");
        }
    bool all_ok = mount_problems.empty() && commands_ok;
    for (size_t i = 0; i < started.size(); ++i) {
        const json &in = found[std::to_string(started[i].pid)];
        json e{{"instance", started[i].id}, {"pid", started[i].pid}, {"port", in.value("port", 0)}};
        if (!owner.empty()) e["owner"] = owner;
        if (!mount_root.empty()) e["lua_mount"] = mount_res[i];
        if (!startup_cmds.empty()) e["commands"] = cmd_res[i];  // Issue #10
        if (!map.empty()) {
            bool ok = !map_res[i].contains("error");
            all_ok = all_ok && ok;
            e["map"] = ok ? json(map) : json(nullptr);
            if (!ok) e["map_error"] = map_res[i]["error"];
            if (map_res[i].contains("fp7137_waited")) e.update(map_res[i]["fp7137_waited"]);  // FP7137
        }
        results.push_back(e);
    }
    // Issue #10: what the clients really run at (the preset's, or the requested frame cap).
    auto cap_text = [](int v) { return v == 0 ? std::string("no cap") : std::to_string(v) + " fps"; };
    std::string fps_note =
        !fps_custom ? "these clients run at most " + std::to_string(fps_cap) + " fps (fps_max, also when unfocused): "
                      "per-frame costs and frame-to-frame behaviour differ from a normal game; pass fps_max to instances_launch "
                      "(or convar_set fps_max) for timing work"
                    : "frame cap set by the fps_max / fps_max_nofocus options: " + cap_text(fps_cap) + " (fps_max), " +
                      cap_text(fps_cap_nofocus) + " when unfocused (fps_max_nofocus); the preset's cap (light 20, normal 30) is "
                      "replaced. A command or launch_args can change it again";
    json res{{"instances", results}, {"game_dir", root}, {"found_by", how}, {"preset", preset}, {"keep", keep},
             {"fps_max", fps_cap}, {"fps_max_nofocus", fps_cap_nofocus},
             {"lua_mount", mount_root.empty() ? json(nullptr) : json(mount_root)},
             {"fps_note", fps_note},
             {"next", "Pass `instance` to target one, or instance_select to make one the default; instances_close when done."}};
    if (!extra_args.empty()) res["launch_args"] = extra_args;  // Issue #10
    if (!mount_problems.empty()) {
        std::string w = "lua_mount " + mount_root + " is NOT active on every client: ";
        for (size_t i = 0; i < mount_problems.size(); ++i) w += (i ? "; " : "") + mount_problems[i].get<std::string>();
        res["warning"] = w + ". Tests on these clients would run the installed addon's code (or no usable Lua): fix it with "
                             "lua_mount {path, instance} then map_load, or close them (instances_close).";
    }
    if (!commands_ok) {  // Issue #10
        std::string w = "a startup command failed on at least one client (see instances[].commands): the clients are not in the state `commands` asked for";
        res["warning"] = res.contains("warning") ? json(res["warning"].get<std::string>() + " " + w) : json(w);
    }
    json fp_res = tool_text(res, !all_ok);  // FP7137
    fp_gate.annotate(fp_res);  // FP7137
    return fp_res;  // FP7137
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
    // Issue #6: which of them is the coordinator while other instances live? Its end moves the role: say so.
    std::string coordinator_before;
    int others_alive = 0;
    {
        bool any_id = false;
        for (auto &l : todo) any_id = any_id || !l.id.empty();
        if (any_id) {
            json lst = game_call("instances_list", json::object(), 5);
            coordinator_before = lst.value("coordinator_id", "");
            std::set<std::string> ending;
            for (auto &l : todo) ending.insert(l.id);
            for (const auto &e : lst.value("instances", json::array()))
                if (e.is_object() && e.value("state", "running") == "running" && !ending.count(e.value("id", ""))) ++others_alive;
        }
    }
    bool coordinator_ended = false;
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
        if (!l.id.empty() && l.id == coordinator_before && others_alive > 0) {
            c["was_coordinator"] = true;
            coordinator_ended = true;
        }
        closed.push_back(c);
        std::lock_guard<std::mutex> lk(g_default_mu);
        if (g_default_instance == l.id) g_default_instance.clear();
    }
    json res{{"closed", closed}};
    if (coordinator_ended) {
        // The others elect a new coordinator within about a second of the port being released: say who took the role.
        std::set<std::string> gone;
        for (const auto &c : closed) gone.insert(c.value("instance", ""));
        std::string moved_to;
        auto t0 = std::chrono::steady_clock::now();
        while (moved_to.empty() && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < g_handover_wait_s + 2) {
            json lst = game_call("instances_list", json::object(), 5);
            std::string id = lst.value("coordinator_id", "");
            if (!id.empty() && !gone.count(id)) moved_to = id;
            else std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        for (auto &c : res["closed"])
            if (c.value("was_coordinator", false)) c["coordinator_moved_to"] = moved_to.empty() ? json(nullptr) : json(moved_to);
        res["warning"] = "a closed client was the coordinator (the directory of instances): " +
                         (moved_to.empty() ? std::string("no other instance has taken the role over yet (calls answer coordinator_changing; retry in a few seconds)")
                                           : "the role moved to " + moved_to + " (it re-registers the others within seconds: calls with `instance` may answer coordinator_changing, retryable)") +
                         ". Other agents' calls were interrupted for that time.";
    }
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

void probe_module_version();

// FP7137: the wait before a map_load goes to the game (lock shared with launches), reported to the client as progress.
void fp7137_gate_map_load(fp7137::Gate &gate, const json &msg, const std::atomic<bool> &cancelled) {
    json a = params_of(msg).value("arguments", json::object());
    double t = a.is_object() && a.contains("timeout_s") && a["timeout_s"].is_number() ? a["timeout_s"].get<double>() : 180.0;
    std::string map = a.is_object() && a.contains("map") && a["map"].is_string() ? a["map"].get<std::string>().substr(0, 64) : "";
    json token = params_of(msg).value("_meta", json::object()).value("progressToken", json());
    std::string target = a.is_object() && a.contains("instance") && a["instance"].is_string() ? a["instance"].get<std::string>() : "";  // FP7137 (issue #7)
    gate.enter(fp7137::Kind::MapLoad, "map:" + map, t, t + 10, &cancelled, [&](double w, const std::string &why) { progress(token, w, 0, why); },
               [target] { return fp7137_loading_games(target); });  // FP7137 (issue #7): other games loading are reported, not waited for
}

// FP7137: after the game answered: say how long the call waited, and keep the slot until a background task ends.
// The slot is given back before the answer goes out, so a client that reads the answer never sees it still held.
void fp7137_finish_map_load(fp7137::Gate &gate, Relay &r, const json &msg) {
    if (!r.reached || !r.response.is_object() || !r.response.contains("result") || !r.response["result"].is_object()) {
        gate.release();
        return;
    }
    json &res = r.response["result"];
    gate.annotate(res);
    json &task = res.contains("task") && res["task"].is_object() ? res["task"] : res;  // legacy: result.task, modern: the result
    if (!task.contains("taskId") || !task["taskId"].is_string()) {
        gate.release();
        return;
    }
    std::string id = task["taskId"].get<std::string>();
    if (gate.waited_s() >= 0.05) task["statusMessage"] = "waited " + std::to_string((int)gate.waited_s()) + " s before the load started: " + gate.reason();
    json meta = params_of(msg).value("_meta", json());
    gate.release_when_done([id, meta] {
        json p{{"taskId", id}};
        if (meta.is_object()) p["_meta"] = meta;
        json m{{"jsonrpc", "2.0"}, {"id", "gmod-mcp-bridge-fp7137"}, {"method", "tasks/get"}, {"params", p}};
        Relay t = relay(m, 20);
        return t.reached && t.response.contains("result") && t.response["result"].value("status", "") == "working";
    });
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
    if (name == kLaunchName) {
        res = launch_tool(args, token);
        // The clients just started may run another module version than the one seen before (or none was seen yet):
        // ask now rather than at the next periodic check, so this very answer carries the warning.
        probe_module_version();
    } else if (name == kCloseName) {
        res = close_tool(args);
    } else {
        res = select_tool(args);
    }
    add_version_warning(res);
    out(local_result(msg, res));
}

// Tool lists the bridge serves: the game's, plus its own tools.
void add_bridge_tools(json &result) {
    if (!result.is_object() || !result.contains("tools") || !result["tools"].is_array()) return;
    for (const auto &t : bridge_tools()) result["tools"].push_back(t);
}

// A call for an instance this session launched, while nothing answers on the base port: if that client crashed, say so
// (the coordinator that would have told is the crashed game itself, or is gone).
json launched_answer(const json &fwd) {
    json args = params_of(fwd).value("arguments", json::object());
    if (!args.is_object() || !args.contains("instance") || !args["instance"].is_string()) return json();
    std::string id = args["instance"].get<std::string>();
    std::lock_guard<std::mutex> lk(g_launch_mu);
    for (const auto &l : g_launched) {
        if (l.id.empty() || l.id != id) continue;
        json ce = launched_crash_error(l, 2500);  // issue #8: the connection broke, the process object follows a moment later
        if (!ce.is_null()) return tool_text(json{{"error", ce}}, true);
        crashinfo::Verdict v;
        uint32_t code = 0;
        double ended = 0;
        if (launched_end(l, v, code, ended))
            return tool_text(json{{"error", {{"code", "instance_gone"},
                                             {"message", crashinfo::quit_message(l.id, l.pid, std::max(0.0, unix_now() - ended), v.evidence.empty() ? "clean exit" : v.evidence[0])}}}},
                             true);
    }
    return json();
}

#ifdef _WIN32
// Issue #8: while a call for a client this session launched is in flight, watches that client's process: when it ends the
// connection is cut (the coordinator that relays the call may be the dead game, or an older module that does not watch) and
// the caller gets the crash answer at once instead of waiting for the call's own timeout.
struct ProcessWatch {
    std::atomic<bool> stop{false};
    std::thread th;
    HANDLE dup = nullptr;
    ProcessWatch(const json &fwd, const std::shared_ptr<Inflight> &cancel) {
        json args = params_of(fwd).value("arguments", json::object());
        if (!args.is_object() || !args.contains("instance") || !args["instance"].is_string()) return;
        std::string id = args["instance"].get<std::string>();
        {
            std::lock_guard<std::mutex> lk(g_launch_mu);
            for (const auto &l : g_launched)
                if (!l.id.empty() && l.id == id && l.h) DuplicateHandle(GetCurrentProcess(), l.h, GetCurrentProcess(), &dup, SYNCHRONIZE, FALSE, 0);
        }
        if (!dup) return;
        th = std::thread([this, cancel] {
            while (!stop) {
                if (WaitForSingleObject(dup, 200) == WAIT_OBJECT_0) {
                    if (!stop) cancel->abort_for_process_end();
                    return;
                }
            }
        });
    }
    ~ProcessWatch() {
        stop = true;
        if (th.joinable()) th.join();
        if (dup) CloseHandle(dup);
    }
};
#endif

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
    fp7137::Gate fp_gate;  // FP7137
    if (tool == "map_load") fp7137_gate_map_load(fp_gate, fwd, cancel->cancelled);  // FP7137
    if (!fp_gate.ok()) {  // FP7137: cancelled while waiting, or the wait would outlast the call's timeout
        { std::lock_guard<std::mutex> lk(g_cancel_mu); g_cancel.erase(key); }  // FP7137
        if (!fp_gate.aborted()) out(local_result(msg, fp_gate.error_result()));  // FP7137
        return;  // FP7137
    }
    bool is_list = method == "tools/list" || method == "prompts/list" || method == "resources/list" || method == "resources/templates/list";
    // No idle limit on client requests: a tool call or tasks/result may stay silent for a long time. The client ends
    // them by cancelling (the bridge then closes the HTTP request).
    const double fp_shift = fp_gate.waited_s() > 0 ? fp_gate.waited_s() + 1 : 0;  // FP7137: keep progress increasing
    auto do_relay = [&] {
        return relay(fwd, kNoIdleLimit, cancel, [fp_shift](const json &m) {  // FP7137 (capture)
            json n = m;  // FP7137
            if (fp_shift > 0 && n.value("method", "") == "notifications/progress" && n.contains("params") && n["params"].is_object() && n["params"].contains("progress") && n["params"]["progress"].is_number()) n["params"]["progress"] = n["params"]["progress"].get<double>() + fp_shift;  // FP7137
            out(n);
        });
    };
#ifdef _WIN32
    std::unique_ptr<ProcessWatch> watch;
    if (method == "tools/call" && tool != "game_quit" && tool != "game_terminate") watch = std::make_unique<ProcessWatch>(fwd, cancel);  // issue #8: those two end the process on purpose
#endif
    Relay r = do_relay();
    if (method == "tools/call" && !cancel->cancelled && !r.aborted) r = ride_out_handover(std::move(r), do_relay, &cancel->cancelled, id);  // issue #6
#ifdef _WIN32
    watch.reset();
#endif
    {
        std::lock_guard<std::mutex> lk(g_cancel_mu);
        g_cancel.erase(key);
    }
    if (tool == "map_load") fp7137_finish_map_load(fp_gate, r, fwd);  // FP7137
    if (cancel->cancelled) return;  // cancelled by the client: no response (late answers are dropped too)
    if (r.aborted) {  // issue #8: the launched client this call waited on died
        json a = launched_answer(fwd);
        out(local_result(msg, a.is_null() ? tool_error("instance_gone", "the client this call was waiting for ended") : a));
        return;
    }

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
            res["instructions"] = offline_prefix() + res["instructions"].get<std::string>();
        } else if (!res.is_object()) {
            res = json{{"capabilities", json::object()},
                       {"serverInfo", {{"name", "gmod-mcp"}, {"version", kBridgeVersion}}},
                       {"instructions", offline_instructions()}};
            if (method == "server/discover") res["supportedVersions"] = all_versions();
        } else if (res.contains("instructions") && res["instructions"].is_string()) {
            res["instructions"] = offline_prefix() + res["instructions"].get<std::string>();
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
        if (!tool.empty()) {  // issue #6: a call for another instance that was answered: several games live
            const json &fa = params_of(fwd).value("arguments", json::object());
            if (fa.is_object() && fa.contains("instance") && fa["instance"].is_string() && r.response.contains("result") &&
                !r.response["result"].value("isError", false))
                g_multi_seen_ms = steady_ms();
        }
        if (tool == "instances_list" && r.response.contains("result") && !r.response["result"].value("isError", false)) {
            json &res = r.response["result"];
            res = tool_text(annotate_instances(res.value("structuredContent", json::object()), params_of(msg).value("arguments", json::object())), false);
        }
        if (!tool.empty() && r.response.contains("result")) add_version_warning(r.response["result"]);
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
                {"note", g_conflict ? "no game instance reachable: port_conflict: " + port_conflict_text()
                                    : "no game instance is running: nothing answers on 127.0.0.1:" + std::to_string(g_port) + " (" + why_down() +
                                          "). instances_launch starts test clients; the user's game needs the -mcp launch option."}};
        out(local_result(msg, tool_text(annotate_instances(sc, params_of(msg).value("arguments", json::object())), false)));
    } else if (method == "tools/call" && !launched_answer(fwd).is_null()) {
        out(local_result(msg, launched_answer(fwd)));
    } else if (method == "tools/call") {
        json text{{"type", "text"}, {"text", not_running_text()}};
        out(local_result(msg, json{{"content", json::array({text})},
                                   {"structuredContent", {{"error", {{"code", unreachable_code()}, {"message", not_running_text()}}}}},
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
void probe_module_version() {
    json m{{"jsonrpc", "2.0"}, {"id", "gmod-mcp-bridge-version"}, {"method", "initialize"},
           {"params", {{"protocolVersion", "2025-06-18"}, {"capabilities", json::object()},
                       {"clientInfo", {{"name", "gmod-mcp-bridge"}, {"version", kBridgeVersion}}}}}};
    Relay r = relay(m, 5.0);
    if (!r.reached || !r.response.contains("result")) return;
    std::string v = r.response["result"].value("serverInfo", json::object()).value("version", "");
    if (v.empty()) return;
    std::lock_guard<std::mutex> lk(g_ver_mu);
    if (v != g_module_version) {
        g_module_version = v;
        if (v != kBridgeVersion) log("module " + v + " in the game, bridge " + kBridgeVersion + ": every tool result will say so");
    }
}

void watcher() {
    Link last = g_link.load();
    int ticks = 0;
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
        if (now == Link::Up && (now != last || ++ticks % 30 == 0)) probe_module_version();
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
    if (const char *p = std::getenv("GMOD_MCP_HANDOVER_WAIT_S")) g_handover_wait_s = std::max(0.0, std::atof(p));
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
    fp7137::set_dir(g_dir);  // FP7137
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

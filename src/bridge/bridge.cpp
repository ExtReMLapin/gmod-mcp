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
#include <cstring>
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
#include "lowmem.h"
#include "modcheck.h"  // Issue #13
#include "gamedir.h"
#include "launch_opts.h"  // Issue #10
#include "fp7137.h"  // FP7137
#include "gmod_mcp_reply.h"
#include "mcp_protocol.h"
#include "next_call.h"
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

const double kFreshLossS = 0.5;  // issue #6: a connection that breaks sooner than this, with no answer, is a dying listener

struct Relay {
    bool reached = false;  // false: game unreachable (why in g_why)
    bool refused = false;  // unreachable because the connection was never made (refused, or no answer to the connect): nothing was sent
    bool lost = false;     // issue #6: the connection was made, then broke before a complete answer: the game may have run the call
    bool lost_fresh = false;  // ... within kFreshLossS of the send and with nothing of an answer: the listener that accepted it was dying
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
    const auto t_sent = std::chrono::steady_clock::now();
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
        r.lost = e == httplib::Error::Read || e == httplib::Error::Write;  // issue #6
        // Issue #6 (BUG-2): a coordinator that was just killed still accepts a connection for a moment, then drops it. When that
        // happens at once and nothing came back, the call never ran: it counts as a refused connection (ridden out).
        r.lost_fresh = r.lost && !decided && std::chrono::duration<double>(std::chrono::steady_clock::now() - t_sent).count() < kFreshLossS;
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
// GMOD_MCP_MODULE_DIR replaces it (the bridge tests; an unusual install layout).
std::string module_dir() {
    if (const char *e = std::getenv("GMOD_MCP_MODULE_DIR"))
        if (*e) return e;
    return exe_dir() + "/../module/" + plat::platform_tag();
}

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

// Issue #3 (Claude Code Code Mode): GMOD_MCP_TASKS=off (default "auto", any other value too). Claude Code declares the
// 2026-07-28 Tasks extension in the capabilities of every call, also for the calls a Code Mode script makes, and the game
// then answers map_load and long waits with a task: the script only gets a "moved to the background" stub. With "off" the
// bridge does not forward that extension, so the game never starts a task on its own.
bool server_tasks_off() {
    const char *v = std::getenv("GMOD_MCP_TASKS");
    if (!v) return false;
    std::string s = v;
    for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    return s == "off";
}

// Removes the Tasks extension from the client capabilities of a 2026-07-28 request (the other capabilities and
// extensions stay).
void strip_tasks_extension(json &msg) {
    if (!server_tasks_off() || !msg.contains("params") || !msg["params"].is_object()) return;
    json &p = msg["params"];
    if (!p.contains("_meta") || !p["_meta"].is_object() || !p["_meta"].contains(kMetaCaps) || !p["_meta"][kMetaCaps].is_object()) return;
    json &caps = p["_meta"][kMetaCaps];
    if (caps.contains("extensions") && caps["extensions"].is_object()) caps["extensions"].erase("io.modelcontextprotocol/tasks");
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

// Adds the warning to a tools/call result as the field structuredContent.version_warning. The text block that is the
// JSON of structuredContent (what the module sends) is rewritten with the same field, put first so that it is read
// first. A result without structuredContent whose first text block is a JSON object (an error such as a cancelled
// task) gets the field inside that object. Issue #3 (Code Mode): Claude Code builds `e.detail` of a thrown error with
// JSON.parse only when the whole error text starts with `{`, so a JSON text is never given a prose block in front of it.
// Any other result keeps its text and gets the warning as a prose block before it.
void add_version_warning(json &result) {
    std::string w = version_warning();
    if (w.empty() || !result.is_object()) return;
    if (!result.contains("content") || !result["content"].is_array()) result["content"] = json::array();
    json &content = result["content"];
    bool rewritten = false;
    if (result.contains("structuredContent") && result["structuredContent"].is_object()) {
        json &sc = result["structuredContent"];
        for (json &c : content) {
            if (!c.is_object() || c.value("type", "") != "text" || !c.contains("text") || !c["text"].is_string()) continue;
            if (json::parse(c["text"].get<std::string>(), nullptr, false) == sc) {
                std::string body = sc.dump(-1, ' ', false, json::error_handler_t::replace);  // {...} without the warning
                c["text"] = "{\"version_warning\":" + json(w).dump(-1, ' ', false, json::error_handler_t::replace) +
                            (body.size() > 2 ? "," + body.substr(1) : "}");
                rewritten = true;
            }
            break;  // only the first text block can be the JSON
        }
        sc["version_warning"] = w;
    }
    if (!rewritten) {
        for (json &c : content) {
            if (!c.is_object() || c.value("type", "") != "text" || !c.contains("text") || !c["text"].is_string()) continue;
            json body = json::parse(c["text"].get<std::string>(), nullptr, false);
            if (body.is_object()) {  // only the first text block can be the JSON
                std::string text = body.dump(-1, ' ', false, json::error_handler_t::replace);
                c["text"] = "{\"version_warning\":" + json(w).dump(-1, ' ', false, json::error_handler_t::replace) +
                            (text.size() > 2 ? "," + text.substr(1) : "}");
                rewritten = true;
            }
            break;
        }
    }
    if (!rewritten) content.insert(content.begin(), json{{"type", "text"}, {"text", w}});
}

// --- Lua error warnings (issue #15) ---
// Every answer of the game carries `_meta["gmod-mcp/lua_errors"]` = {instance, cursor, recent} (cursor: how many errors the game has
// raised so far; recent: how many in the last 5 s). This bridge serves one caller, so it is the one place that knows what THIS caller
// has seen: it remembers the cursor of each game at the caller's last answer, and when the game raised a burst of errors since (20 or
// more in the last 5 s) it asks the game what they were and puts a `warnings` entry on the answer, so a hook that fails a hundred times
// a second does not go on unnoticed. Without a burst nothing is asked and the answer only loses the _meta key.
json game_call(const std::string &tool, json args, double timeout_s);  // defined below
constexpr uint64_t kErrorBurst = 20;
std::mutex g_errwarn_mu;
std::map<std::string, uint64_t> g_errors_seen;  // game id -> its error cursor at this caller's last answer

// Puts `key: value` first in structuredContent and in the JSON text block that mirrors it. Returns false when the result has no
// JSON object to carry it (then nothing is changed).
bool put_top_field(json &result, const std::string &key, const json &value) {
    if (!result.is_object() || !result.contains("content") || !result["content"].is_array()) return false;
    std::string v = value.dump(-1, ' ', false, json::error_handler_t::replace);
    if (result.contains("structuredContent") && result["structuredContent"].is_object()) {
        json &sc = result["structuredContent"];
        for (json &c : result["content"]) {
            if (!c.is_object() || c.value("type", "") != "text" || !c.contains("text") || !c["text"].is_string()) continue;
            if (json::parse(c["text"].get<std::string>(), nullptr, false) == sc) {
                std::string body = sc.dump(-1, ' ', false, json::error_handler_t::replace);
                c["text"] = "{" + json(key).dump() + ":" + v + (body.size() > 2 ? "," + body.substr(1) : "}");
            }
            break;  // only the first text block can be the JSON
        }
        sc[key] = value;
        return true;
    }
    for (json &c : result["content"]) {
        if (!c.is_object() || c.value("type", "") != "text" || !c.contains("text") || !c["text"].is_string()) continue;
        json body = json::parse(c["text"].get<std::string>(), nullptr, false);
        if (body.is_object()) {
            std::string text = body.dump(-1, ' ', false, json::error_handler_t::replace);
            c["text"] = "{" + json(key).dump() + ":" + v + (text.size() > 2 ? "," + text.substr(1) : "}");
            return true;
        }
        break;
    }
    return false;
}

double round1(double x) { return std::round(x * 10.0) / 10.0; }

void add_lua_error_warnings(json &result, const std::string &tool) {
    static const char *kKey = "gmod-mcp/lua_errors";
    if (!result.is_object() || !result.contains("_meta") || !result["_meta"].is_object() || !result["_meta"].contains(kKey)) return;
    json m = result["_meta"][kKey];
    result["_meta"].erase(kKey);  // private to the bridge
    if (result["_meta"].empty()) result.erase("_meta");
    if (!m.is_object()) return;
    std::string inst = m.value("instance", "");
    uint64_t cursor = m.value("cursor", (uint64_t)0), recent = m.value("recent", (uint64_t)0);
    uint64_t last;
    {
        std::lock_guard<std::mutex> lk(g_errwarn_mu);
        auto it = g_errors_seen.find(inst);
        // A game this caller has not met: only what the burst itself holds counts, the history before is not "since your last call".
        last = it != g_errors_seen.end() ? it->second : (cursor > recent ? cursor - recent : 0);
        g_errors_seen[inst] = cursor;
    }
    // The caller reading the errors is hearing about them: no warning on errors_since / errors_clear themselves.
    if (tool == "errors_since" || tool == "errors_clear" || cursor <= last || recent < kErrorBurst) return;
    json args{{"cursor", last}, {"group_by", "message"}, {"limit", 3}};
    if (!inst.empty()) args["instance"] = inst;
    json sc = game_call("errors_since", args, 5.0);
    if (!sc.is_object() || !sc.contains("groups") || !sc["groups"].is_array() || sc["groups"].empty()) return;
    json warnings = json::array();
    double first = 1e300, lastt = 0;
    for (const auto &g : sc["groups"]) {
        first = std::min(first, g.value("first_time", 0.0));
        lastt = std::max(lastt, g.value("last_time", 0.0));
    }
    uint64_t total = cursor - last;
    double seconds = round1(std::max(0.0, lastt - first));
    for (const auto &g : sc["groups"]) {
        json w{{"code", "lua_error_spam"}, {"count", g.value("count", 0)}, {"errors_since_your_last_call", total}, {"seconds", seconds},
               {"message", g.value("message", "")}, {"instance", inst}};
        if (g.contains("source")) w["source"] = g["source"];
        if (g.contains("hook")) w["hook"] = g["hook"];
        std::string text = std::to_string(total) + " Lua errors in the last " + std::to_string((int)std::ceil(std::max(seconds, 1.0))) + " s on " +
                           (inst.empty() ? "the game" : inst) + " since your last call; this message came " + std::to_string(g.value("count", 0)) + " times";
        if (g.contains("source")) text += ", raised at " + g["source"].get<std::string>();
        if (g.contains("hook")) text += ", by the hook " + g["hook"].get<std::string>();
        text += ". GMod never removes a hook that errors: if it is a hook you added, remove it with hook.Remove now. "
                "errors_since {group_by: \"message\"} lists what repeats.";
        w["text"] = text;
        warnings.push_back(std::move(w));
    }
    put_top_field(result, "warnings", warnings);
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
    bool private_desktop = false;  // issue #14: started on the session's private desktop
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

std::mutex g_peers_mu;
bool g_peers_known = false;           // the last list that showed several games named their members
std::vector<uint32_t> g_peer_pids;    // their pids (0 = unknown): the running games other than the coordinator, who would take its role over

// instances_list answer: several live instances seen now means a coordinator change is worth waiting for; a list that shows one
// game (or none) means there is nothing to wait for (issue #6). A list of a coordinator that has just changed is incomplete: ignored.
void note_instances(const json &sc) {
    if (!sc.is_object() || !sc.contains("instances") || !sc["instances"].is_array() || sc.value("coordinator_changing", false)) return;
    int running = 0;
    std::string coord = sc.value("coordinator_id", "");
    std::vector<uint32_t> members;
    for (const auto &e : sc["instances"])
        if (e.is_object() && e.value("state", "running") == "running") {
            ++running;
            if (e.value("role", "") != "coordinator" && e.value("id", "") != coord) members.push_back(e.value("pid", 0u));
        }
    std::lock_guard<std::mutex> lk(g_peers_mu);
    if (running >= 2) {
        g_multi_seen_ms = steady_ms();
        g_peers_known = true;
        g_peer_pids = members;
    } else {
        g_multi_seen_ms = 0;
        g_peers_known = false;
        g_peer_pids.clear();
    }
}

// Issue #6 (BUG-3): does a game that could take the coordinator role over still run? Unknown (no list seen) counts as yes.
bool peers_may_live() {
    std::lock_guard<std::mutex> lk(g_peers_mu);
    if (!g_peers_known) return true;
    for (uint32_t pid : g_peer_pids)
        if (pid == 0 || plat::process_start_stamp(pid) != 0) return true;
    return false;
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
    if (launched_client_alive()) return true;
    int64_t t = g_multi_seen_ms.load();
    if (t == 0 || steady_ms() - t >= 30000) return false;
    if (peers_may_live()) return true;
    g_multi_seen_ms = 0;  // issue #6 (BUG-3): the games that could take the role over are gone: nothing to wait for
    return false;
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
         "main instance). Each starts with -multirun, gmod-mcp on, a small window and a frame cap, on a private Windows desktop (below), waits until it answers, "
         "then loads `map` through map_load (never +map: a game loading a map in the background can grab the mouse). "
         "Returns their instance ids and pids: pass an id as `instance` on every call. "
         "Without keep: true they end with this bridge session (when it ends, the clients it started are closed, also when the bridge is killed: "
         "they run in a job object that dies with it); with keep: true they "
         "outlive it, and a later session closes them by `owner` (below) or by pid / game_terminate. "
         "They close when this session ends unless keep is true; close them earlier with instances_close (not game_quit: "
         "it keeps their exit short; a clean quit is safe too, see the convar note below). Sub-agents of one Claude Code session share "
         "this bridge: give each one's clients an `owner` label and close them with instances_close {owner}, never "
         "{all: true}, which would close the other sub-agents' clients too. The `owner` label goes to the game (-mcp_owner) and "
         "back in its heartbeat: instances_list shows `owner` on every instance whichever bridge session launched it, and "
         "instances_close {owner} also closes keep: true test clients of an EARLIER session that carry that label (only test "
         "clients started by instances_launch, never the user's own game or an instance without the tag). The game is found through Steam; pass "
         "game_dir (the folder holding gmod_win64.exe or bin/win64/gmod.exe) if it is elsewhere. Windows only for now.\n"
         "Presets: 'light' (default: 640x360, 20 fps; for most tests, NOT for performance measurements: the 20 fps cap "
         "hides per-frame cost) and 'normal' (640x360, 30 fps; for rendering checks, screenshots and frame-time sensitive "
         "tests). sound: false (-nosound) changes what the engine reports about sounds: SoundDuration of compressed "
         "(ADPCM) WAV files differs (0.49 s instead of 1.24 s seen); pass sound: true for sound tests.\n"
         // Issue #14
         "Desktop (Windows): by default each client runs on a private Windows desktop of this bridge session (desktop: \"private\"; "
         "result desktop, desktop_name). A starting game makes every game on ITS desktop release and restore its GPU resources "
         "(the user's own game freezes about 10 s and its render targets are rebuilt empty); a client on another desktop never "
         "finds the user's windows, so the user's game is not disturbed. Consequences: the client has no visible window and no taskbar "
         "button (nothing to close by hand: instances_close), it never takes the user's focus, and sound is OFF unless you pass "
         "sound: true (nobody sees it, and its audio would still reach the speakers). ui_click, ui_key and screenshot still work "
         "(posted window messages and the engine's own capture: no real input and no screen grab). Clients of one session share "
         "that desktop, so they still disturb each other as described below. desktop: \"shared\" starts them on the user's desktop "
         "(visible window, the foreground handed back to the user's window): use it only to look at the window.\n"
         // Issue #13
         "Before starting anything the module installed in the game folder (lua/bin) is compared with the one this plugin ships: "
         "an older one is refused with module_outdated and the update steps (a client would run it and only say so afterwards), unless "
         "ignore_outdated_module: true. instances_list and the game_not_running error show module_outdated while no game runs.\n"
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
         "of one core, 1.55 GB of commit (virtual memory) and 1.2 GB of RAM; normal 24% and the same memory. Eight clients: "
         "about 1.5 cores and 12 GB. Memory is the limit, not CPU: a machine out of commit makes every game crash "
         "(0xC000041D, no dump), so before starting anything this call checks the free commit and free RAM and refuses with "
         "the error low_memory {details: free_commit_mb, free_ram_mb, running_clients (all sessions: the "
         "registered instances, this bridge's clients and every process of the game's executable, so clients another session started "
         "that have not registered yet count), registered_clients, game_processes, per_client_mb, count, "
         "needed_commit_mb, needed_ram_mb} when they do not cover the clients asked for (1.6 GB of commit each plus 1 GB "
         "for the rest of the machine, 1.2 GB of RAM each); nothing is started then. Close clients you no longer use "
         "(instances_close), start fewer, or pass ignore_low_memory: true to start anyway. The frame cap is what saves CPU (30 to 20 fps: -20%); sound: false saves about 12% more. Window "
         "size barely matters, and a client narrower than 640 pixels crashes while loading a map (integer division by zero "
         "in client.dll at client Lua startup: seen at 480x270 and 320x240; it only survived with the HUD turned off). "
         "-textmode and lower graphics settings were measured: no real gain (and graphics settings would be saved into the "
         "user's own), so neither is offered.\n"
         "Result: {instances: [{instance, pid, port, owner?, commands? [{cmd, ok, error?}], map? (or map: null + map_error)}], game_dir, found_by, preset, "
         "keep, desktop, desktop_name?, sound, module?, fps_max, fps_max_nofocus, fps_note, launch_args?, lua_mount}. If they do not all answer within timeout_s: error timeout with silent_pids "
         "(started but never answered: close them with instances_close {pids}) and answered [{instance, pid}]. A client that "
         "crashed during startup or its map load is reported as client_crashed (startup: the whole call's error; map load: that "
         "client's map_error), with the minidump file name in details.dump (for a human) and exit_code: launch a replacement. "
         "With count > 1 the other clients of the call are not closed when one dies at startup: the error lists the ones still "
         "running in details.still_running [{pid, state: answering or still starting, instance?}] and a top-level warning; "
         "close them (instances_close) or use them. When the clients started but a map, a startup command or the lua_mount did not work, "
         "the call is isError with the whole result above plus error {code: launch_incomplete, message}. Example: "
         "{\"count\": 2, \"map\": \"gm_construct\", \"owner\": \"fork-a\"}, then pass each returned instance id as "
         "`instance`.\n"
         // FP7137 (git grep FP7137: agent-facing text A1-A3, see docs/FP7137.md)
         "Starting a game makes every other running Garry's Mod release its GPU resources for 2-10 s, and a map load that "
         "ends in that window crashes that game (engine bug, Facepunch garrysmod-issues #7137). So the bridge serialises "
         "the launches and map loads that go through it, from any session: this call first waits while any game loads a "
         "map, and with `map` the maps load after a 15 s settle delay, 2 at a time across every session (a map_load of another session counts). \"Any game\" includes loads the bridge did "
         "not start (console `map`, changelevel, Lua RunConsoleCommand, a script talking to the game): every game reports "
         "when it is loading a map (instances_list `loading`), and waited_reason then starts with map_load_outside_bridge. "
         "A call that waited reports waited_s and "
         "waited_reason (and progress when your client sent a progress token). If the wait would exceed timeout_s it fails "
         "with error map_load_in_progress and starts nothing: retry later, or with a larger timeout_s. Not covered: a game "
         "started by hand (not by this call) and a load that begins while a launch is already creating its processes "
         "(only launches and map_load calls of the bridge wait, a load started by other means is never held back).\n"
         // Issue #8 (chaos test G1)
         "A client that is frozen (instances_list state unresponsive or hung: suspended, crash dialog, a load that never ends) keeps "
         "a new client from registering (engine FP7137 handshake), so a launch beside it ends with `0 of N clients answered`: "
         "before starting anything this call refuses with peer_unhealthy {details.unhealthy: [{instance, pid, state}]}, naming "
         "the way out (instances_close {pids} for a client this session started, game_terminate {instance} for any), unless "
         "ignore_unhealthy: true; a timeout also names the stuck peers it saw meanwhile (details.unhealthy_peers).\n"
         "When a program that is not gmod-mcp holds the base port (port_conflict, see status), the launch is refused up front with the fix "
         "(GMOD_MCP_PORT) and nothing is started: the clients would register on the next port, where this bridge cannot see them.\n"
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
            {"sound", {{"type", "boolean"}, {"description", "Sound on. Default false on the private desktop (invisible clients stay silent), true with desktop: shared. Pass true for sound tests (SoundDuration differs without sound)."}}},
            {"desktop", {{"type", "string"}, {"enum", json::array({"private", "shared"})}, {"description", "private (default): a Windows desktop of this session, so the user's own game is not disturbed (no visible window, no focus taken). shared: the user's desktop, visible window."}}},  // Issue #14
            {"ignore_outdated_module", {{"type", "boolean"}, {"description", "Start even when the module installed in the game folder is older than the one this plugin ships (error module_outdated otherwise). Default false."}}},  // Issue #13
            {"fps_max", {{"type", "integer"}, {"minimum", 0}, {"maximum", 1000},  // Issue #10
                         {"description", "Frame cap of these clients (fps_max): 0 = no cap, or 10 to 1000. Default: the preset's (light 20, normal 30). Given back to the user's value on a clean quit."}}},
            {"fps_max_nofocus", {{"type", "integer"}, {"minimum", 0}, {"maximum", 1000},  // Issue #10
                                 {"description", "Frame cap when the window has no focus (fps_max_nofocus): 0 = no cap, or 10 to 1000. Default: the same as fps_max."}}},
            {"commands", {{"type", "array"}, {"maxItems", 32}, {"items", {{"type", "string"}, {"maxLength", 256}}},  // Issue #10
                          {"description", "Console commands run on each client once it answers, before `map` loads, e.g. [\"fps_max 300\", \"snd_mute_losefocus 0\"]. quit, exit, connect and map changes are refused. Archived convars they change are given back on a clean quit."}}},
            {"launch_args", {{"type", "array"}, {"maxItems", 32}, {"items", {{"type", "string"}, {"maxLength", 160}}},  // Issue #10
                             {"description", "Extra engine command-line tokens, one per element, e.g. [\"-noborder\", \"+cl_showfps\", \"1\"]. Refused: what gmod-mcp sets itself (-multirun, -mcp*, -w, -h, window mode, -dxlevel, -condebug, -game), +connect, +map. Options: https://developer.valvesoftware.com/wiki/Command_line_options"}}},
            {"keep", {{"type", "boolean"}, {"description", "Keep them running after this session ends (default false)."}}},
            {"ignore_unhealthy", {{"type", "boolean"}, {"description", "Start even when a running client is unresponsive or hung (error peer_unhealthy otherwise: a frozen client keeps a new one from registering, so the launch would time out). Default false."}}},
            {"ignore_low_memory", {{"type", "boolean"}, {"description", "Start even when the free memory does not cover the clients asked for (error low_memory otherwise). Default false: a machine out of memory crashes every game on it."}}},
            {"owner", {{"type", "string"}, {"maxLength", 64}, {"description", "Label recorded with these clients (e.g. your sub-agent's name or task), also on their command line and in instances_list; instances_close {owner} then closes only them, even from a later bridge session when keep is true."}}},
            {"lua_mount", {{"type", "string"}, {"maxLength", 1024},
                         {"description", "Absolute folder whose Lua these clients load before every addon's (e.g. your git worktree of "
                                         "the addon: its root or its lua/ folder). Applies from their first map load; see lua_mount. "
                                         "Each engine is asked what it really has BEFORE any map loads (instances[i].lua_mount.active); a missing mount is "
                                         "applied again, and a client that still has none gets no map (map_error lua_mount_not_active, "
                                         "isError, warning). Windows only."}}},
            {"allow_shadow", {{"type", "boolean"}, {"description", "With lua_mount: accept a folder that replaces files of the game's own base Lua "
                                                                                 "(lua/includes/init.lua...). Refused by default (shadows_base_lua): such a client hangs in its load."}}},
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
         "By id, pid or all, only this bridge session's clients: other games (the user's own, another session's) are refused, close "
         "those only if the user asks, with game_terminate. By owner label it also closes keep: true test clients of an earlier "
         "session (a bridge restarted, another agent) that carry that label (instances_list shows it as `owner`; closed[i]."
         "launched_by_other_session is true), but never a game that is not a test client or one without the tag. Sub-agents of one Claude Code session share it: all: true closes their clients too. The selectors "
         "add up (instances, pids, owner). Result: {closed: [{pid, instance?, owner?}], already_gone?: [{pid, instance?, owner?}] (processes that had ended before the call: nothing was closed), refused?: [ids, pids or "
         "\"owner:<label>\" not started by this session or already closed], why_refused?}; an error (code refused, the same fields beside it) only when nothing was "
         "closed. Closing the client that is the coordinator (the game on the base port, see instances_list: role, "
         "coordinator_warning) while other games live moves that role: the answer says so (closed[i].was_coordinator, "
         "coordinator_moved_to, warning) and calls of other agents answer coordinator_changing for a few seconds. "
         "Example: {\"owner\": \"fork-a\"}."},
        {"inputSchema",
         {{"type", "object"},
          {"properties",
           {{"instances", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Instance ids to close."}}},
            {"pids", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"description", "Process ids to close (as returned by instances_launch)."}}},
            {"owner", {{"type", "string"}, {"description", "Close every client launched with this owner label, including keep: true test clients of an earlier bridge session that carry it."}}},
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
         "calls too (they would act on its game). Pass `instance` on every call instead. Result: {default_instance, previous}. An id that instances_list does not show is refused (unknown_instance, the default stays); when the list cannot be read the id is accepted with a warning."},
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
        crashinfo::read_dump_facts(crashinfo::dump_dirs(l.root + "/garrysmod"), l.slot->verdict);  // issue #8: the faulting module and the stack's callers
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
    crashinfo::add_fault_details(details, v);  // issue #8: fault, stack_candidates
    std::string message = crashinfo::crash_message("client", l.id.empty() ? "started by this session" : l.id, l.pid, ago, v, true, code);
    lowmem::annotate_crash(lowmem::current(), details, message);  // issue #8: free memory now, and a hint when it is low
    return json{{"code", crashinfo::crash_code("client")}, {"message", message}, {"details", details}};
}

// instances_list, completed by the bridge: the owner label of each client it launched, the clients it launched that
// never answered (gmod-mcp not loaded, still starting), and an optional owner filter. `sc` is the game's answer
// (or an empty list when no game answers).
json annotate_instances(json sc, const json &args, bool from_game = true) {
    std::string want = args.value("owner", "");
    std::set<uint32_t> listed;
    json kept = json::array();
    if (from_game) note_instances(sc);
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
                e["desktop"] = l.private_desktop ? "private" : "shared";  // issue #14
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
                json mem = lowmem::memory_json(lowmem::current());  // issue #8
                if (!mem.is_null()) e["crash"]["memory"] = mem;
                sc["instances"].push_back(e);
            }
            continue;
        }
#endif
        json x{{"pid", l.pid}, {"note", "launched by this session, not answering: still starting, or gmod-mcp did not load in it "
                                        "(see garrysmod/data/mcpagent/last_crash.txt). instances_close {pids} closes it."}};
        if (!l.owner.empty()) x["owner"] = l.owner;
        x["desktop"] = l.private_desktop ? "private" : "shared";  // issue #14
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
// A call refused because another launch or map load is in progress (FP7137): the same call a little later, with room to wait
// (the settle delay after a game starts is 15 s). `args` is the call's arguments, `fallback_timeout_s` its default timeout.
json retry_later(json refused, const std::string &tool, const json &args, double fallback_timeout_s) {
    json a = args.is_object() ? args : json::object();
    double old = a.contains("timeout_s") && a["timeout_s"].is_number() ? a["timeout_s"].get<double>() : fallback_timeout_s;
    a["timeout_s"] = std::min(900.0, std::max(60.0, 2 * old));
    next_call::attach(refused, next_call::make(tool, a, 15));
    return refused;
}

// The answer to a tool call while the game is unreachable: the module's error shape, as text and structuredContent.
json offline_module_info();  // issue #13, below

json not_running_error() {
    json details{{"host", "127.0.0.1"}, {"port", g_port}, {"reason", why_down()}};
    details.update(offline_module_info());  // issue #13: module_outdated / module_check, when the game folder is known
    return tool_text(json{{"error", {{"code", unreachable_code()}, {"message", not_running_text()}, {"details", details}}}}, true);
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
        if (!r.reached && r.lost && !r.lost_fresh && !r.refused && handover_expected()) {
            // Issue #6: the call was in flight when the coordinator went away while other games live. The target game may well
            // have run it (a map load, a Lua job): repeating it here could run it twice, and game_not_running would be a lie.
            json e{{"error", {{"code", "relay_lost"},
                              {"message", "the connection to the coordinator (the game on port " + std::to_string(g_port) + ") broke while this call was in flight, "
                                          "and other games are running: the coordinator role is moving. The call MAY HAVE BEEN EXECUTED by its target game (a map "
                                          "load or a Lua job may have started or finished): check the game's state (status, instances_list, lua_job_status) before "
                                          "repeating a call that is not idempotent. The game itself is not closed; calls work again within a few seconds."},
                              {"details", {{"retry_after_s", 3.0}, {"maybe_executed", true}}}}}};
            r.reached = true;
            r.status = 200;
            r.response = rpc_result(id, tool_text(e, true));
            return r;
        }
        if (!r.reached) wait = (r.refused || r.lost_fresh) && handover_expected();
        else if (r.response.is_object() && r.response.contains("result")) wait = coordinator_changing_result(r.response["result"], &retry);
        if (!wait) return r;
        if (waited() >= g_handover_wait_s || (cancelled && cancelled->load())) {
            if (!r.reached) {
                json e{{"error", {{"code", "coordinator_changing"},
                                  {"message", "the coordinator (the game on port " + std::to_string(g_port) + ") stopped answering while other instances "
                                              "live, and none has taken the role over yet (waited " + std::to_string((int)waited()) + " s). Retry in a few seconds; " +
                                              (r.lost_fresh ? "the call was cut at once, before any answer: it most likely did not run." : "nothing was sent to the game.")},
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

// --- Issue #13: the module installed in the game against the one this bridge ships ---
const char *module_file_name() {
#ifdef _WIN32
    return "gmsv_mcpagent_win64.dll";
#else
    return "gmsv_mcpagent_linux64.dll";
#endif
}

struct ModuleCheck {
    modcheck::Verdict verdict;
    std::string shipped_file, installed_file;
};

// Reads the two module files (nothing is loaded or written). The shipped one is <module dir>/garrysmod/lua/bin/<name>; the installed one
// <game folder>/garrysmod/lua/bin/<name>.
ModuleCheck check_installed_module(const std::string &game_root) {
    ModuleCheck c;
    c.shipped_file = module_dir() + "/garrysmod/lua/bin/" + module_file_name();
    c.installed_file = game_root + "/garrysmod/lua/bin/" + module_file_name();
    std::string shipped, installed;
    bool sr = plat::read_file(c.shipped_file, shipped);
    bool ir = plat::read_file(c.installed_file, installed);
    c.verdict = modcheck::decide(sr, sr ? modcheck::find_version(shipped) : "", ir, ir ? modcheck::find_version(installed) : "");
    return c;
}

json module_json(const ModuleCheck &c) {
    json m{{"state", c.verdict.name()}};
    if (!c.verdict.shipped.empty()) m["shipped"] = c.verdict.shipped;
    if (!c.verdict.installed.empty()) m["installed"] = c.verdict.installed;
    m["shipped_file"] = c.shipped_file;
    m["installed_file"] = c.installed_file;
    return m;
}

// The steps that bring the installed module up to date (what the gmod-mcp skill does, section 3).
std::string module_update_steps(const ModuleCheck &c, const std::string &game_root) {
    std::string base = c.shipped_file.substr(0, c.shipped_file.size() - std::string("/lua/bin/").size() - std::string(module_file_name()).size());  // <module dir>/garrysmod
    return "Update: quit every game that runs gmod-mcp (a loaded module cannot be replaced), then copy " + c.shipped_file + " to " + c.installed_file +
           " and " + base + "/lua/autorun/server/mcpagent_boot.lua to " + game_root + "/garrysmod/lua/autorun/server/mcpagent_boot.lua (the gmod-mcp skill does it: "
           "`gmod-mcp-bridge --module-dir` prints the shipped folder), then call this again.";
}

// While no game runs: module_outdated (only when the versions could be compared) and module_check, for the game folder the bridge knows.
// An empty object when the folder is not known.
json offline_module_info() {
    // The files are a few MB each and every failed call asks: read them again at most every 3 s.
    static std::mutex mu;
    static std::chrono::steady_clock::time_point at;
    static json last;
    std::lock_guard<std::mutex> lk(mu);
    if (!last.is_null() && std::chrono::steady_clock::now() - at < std::chrono::seconds(3)) return last;
    std::string how, root = find_game_root("", how);
    json r = json::object();
    at = std::chrono::steady_clock::now();
    last = r;
    if (root.empty()) return r;
    ModuleCheck c = check_installed_module(root);
    using S = modcheck::State;
    if (c.verdict.state == S::Outdated) r["module_outdated"] = true;
    else if (c.verdict.state == S::Current || c.verdict.state == S::Newer) r["module_outdated"] = false;
    if (c.verdict.state != S::Unknown) r["module_check"] = module_json(c);
    last = r;
    return r;
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
    auto same = [](const std::string &a, const std::string &b) { return lua_mount::same_path(a, b); };
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
        if (answer.value("folder_missing", false)) {  // Issue #9: renamed or deleted since it was mounted
            v["active"] = false;
            v["error_code"] = "mount_folder_missing";
            v["error"] = "mount_folder_missing: the mounted folder (or its lua/) does not exist any more: the game serves nothing from it";
            return v;
        }
        // Issue #9: the engine lists the folder, but its file system may not be able to read it (UNC, junction, long path...):
        // the game then asks the module's own bookkeeping, not the file system, and runs the installed code.
        if (answer.contains("engine_serves_folder") && answer["engine_serves_folder"].is_boolean()) {
            v["engine_serves_folder"] = answer["engine_serves_folder"];
            if (answer["engine_serves_folder"] == json(false)) {
                v["active"] = false;
                v["error_code"] = "mount_not_effective";
                v["error"] = "mount_not_effective: the engine lists the folder but its file system serves none of the files it was asked "
                             "for (a path it cannot read: UNC, junction, over-long or non-ASCII), so the game runs the installed addon's code";
            }
        }
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
    // Live verification of 0.3.7: with no game this was a second refused connection (about 1 s on Windows) before the call's own: map_load
    // answered game_not_running after 3 s, status after 1 s. The last call said nothing listens: there is nothing to ask (a game that started
    // since only goes unreported for this call: the link is refreshed by the call itself).
    if (g_link == Link::Down) return ids;
    json lst = game_call("instances_list", json::object(), 3);  // FP7137: an unreachable game is an error object: no ids
    if (!lst.is_object() || !lst.contains("instances") || !lst["instances"].is_array()) return ids;  // FP7137
    for (const auto &in : lst["instances"])  // FP7137
        if (in.is_object() && in.value("loading", false) && in.value("state", "running") == "running" && in.value("id", "") != except)  // FP7137
            ids.push_back(in.value("id", "?"));  // FP7137
    return ids;  // FP7137
}  // FP7137

// Issue #8 (chaos test G1): the running instances whose main thread or whole process is stuck (state "unresponsive" or "hung").
// A frozen client keeps the engine from letting a new client register (FP7137 handshake): a launch then times out with
// "0 of 1 clients answered". [{instance, pid, state}]
json unhealthy_peers(const json &lst) {
    json out = json::array();
    if (!lst.is_object() || !lst.contains("instances") || !lst["instances"].is_array()) return out;
    for (const auto &e : lst["instances"]) {
        if (!e.is_object()) continue;
        std::string st = e.value("state", "");
        if (st != "unresponsive" && st != "hung") continue;
        out.push_back(json{{"instance", e.value("id", "?")}, {"pid", e.value("pid", 0u)}, {"state", st}});
    }
    return out;
}

std::string describe_unhealthy(const json &peers) {
    std::string t;
    for (const auto &u : peers) t += (t.empty() ? "" : ", ") + u.value("instance", std::string("?")) + " (pid " + std::to_string(u.value("pid", 0u)) + ") is " + u.value("state", std::string("?"));
    return t;
}

#ifdef _WIN32
// Issue #8 (chaos test G4): pids of the games running this game's executable (any session's client, registered with the
// directory or still starting). Matched by the full image path (case-insensitive), so another copy of the game is not counted.
// Each game also starts helper processes of the same executable (`--type=renderer`, `--type=utility`: about 4 on a live game, 2 on a
// light one), which are not games and each hold memory of their own to a small degree only: a process whose parent is itself a
// process of the game (created before it: the pid may have been reused) is left out. Seen live: 14 processes for 3 games.
std::set<uint32_t> game_process_pids(const std::string &exe) {
    std::set<uint32_t> out;
    std::map<uint32_t, uint32_t> parent;
    auto norm = [](std::wstring w) {
        for (auto &c : w) c = c == L'/' ? L'\\' : (wchar_t)towlower(c);
        return w;
    };
    auto created = [](uint32_t pid) -> uint64_t {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) return 0;
        FILETIME c{}, e{}, k{}, u{};
        uint64_t t = GetProcessTimes(h, &c, &e, &k, &u) ? (((uint64_t)c.dwHighDateTime << 32) | c.dwLowDateTime) : 0;
        CloseHandle(h);
        return t;
    };
    std::wstring want = norm(utf8_to_wide(exe));
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof e;
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.th32ProcessID);
        if (!h) continue;
        wchar_t buf[2048];
        DWORD n = 2048;
        if (QueryFullProcessImageNameW(h, 0, buf, &n) && norm(std::wstring(buf, n)) == want) {
            out.insert(e.th32ProcessID);
            parent[e.th32ProcessID] = e.th32ParentProcessID;
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    std::set<uint32_t> games;
    for (uint32_t pid : out) {
        auto pp = parent.find(pid);
        bool helper = false;
        if (pp != parent.end() && out.count(pp->second) && pp->second != pid) {
            uint64_t tp = created(pp->second), tc = created(pid);
            helper = tp != 0 && tc != 0 && tp <= tc;  // the parent is a game process that started first: this is its helper
        }
        if (!helper) games.insert(pid);
    }
    return games;
}
#endif

#ifdef _WIN32
// Issue #8 (live chaos test): the clients this session launches without keep live in a job object that dies with the bridge
// (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE), so a bridge killed by TerminateProcess / taskkill /F (which runs no exit code) no longer
// leaves them running for ever. The game starts helper processes of its own executable: they join the job (children inherit it; no
// breakaway flag is set, and the client is created suspended so that no child can start before it is in the job). Clients started
// with keep: true are not put in it. Null when the job could not be created (the clients are then started as before).
HANDLE client_job() {
    static HANDLE job = [] {
        HANDLE j = CreateJobObjectW(nullptr, nullptr);
        if (!j) return (HANDLE) nullptr;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(j, JobObjectExtendedLimitInformation, &info, sizeof info)) {
            CloseHandle(j);
            return (HANDLE) nullptr;
        }
        return j;
    }();
    return job;
}

// Issue #6: a map_load whose answer never came (the connection to the coordinator broke while the role moved): the client may have
// loaded the map anyway (seen live: maps loaded with the lua_mount active while instances_launch reported game_not_running).
// Polls its status until it is not loading any more. Returns {map} when it sits on `map`, null when it does not, or when it ended.
json recover_lost_map_load(const Launched &l, const std::string &map, double budget_s) {
    auto t0 = std::chrono::steady_clock::now();
    int idle = 0;  // consecutive readings that say "not loading, another map"
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < budget_s) {
        if (l.h && WaitForSingleObject(l.h, 0) == WAIT_OBJECT_0) return json();
        json st = game_call("status", json{{"instance", l.id}}, 10);
        if (!st.is_object() || st.contains("error")) {
            idle = 0;  // the answer itself may be lost again: look again
        } else if (st.value("loading", false)) {
            idle = 0;
        } else if (st.contains("map") && st["map"].is_string() && st["map"].get<std::string>() == map) {
            return json{{"map", map},
                        {"note", "the answer to map_load was lost (the coordinator role moved while it ran); the client reports the map loaded"}};
        } else if (++idle >= 2) {
            return json();
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return json();
}

// --- Issue #14: the private desktop ---
// A starting game sends the engine's "another app is initializing" message (WM_COPYDATA) to every window of ITS OWN desktop that carries
// the game's marker child, and each receiver releases and restores its GPU resources: the user's own game freezes about 10 s. A client started
// on another desktop of the same window station never finds those windows. One desktop per bridge session, created at the first launch and kept
// while a client started on it still runs; closed (CloseDesktop: the desktop goes away with its last process) after instances_close and at
// the end of the session. Clients of the session still see each other (they share the desktop).
std::mutex g_desk_mu;
HDESK g_desk = nullptr;
std::string g_desk_name;               // "gmod-mcp-<bridge pid>"
std::string g_desk_station;            // the window station of this process ("WinSta0" for a normal session)
std::atomic<int> g_desk_launching{0};  // launches in progress: the desktop is never closed under them

// The desktop the clients start on, created if needed. False with `err` when the system refuses.
bool private_desktop(std::string &name, std::string &station, std::string &err) {
    std::lock_guard<std::mutex> lk(g_desk_mu);
    if (!g_desk) {
        char st[256] = {};
        DWORD need = 0;
        HWINSTA ws = GetProcessWindowStation();
        if (!ws || !GetUserObjectInformationA(ws, UOI_NAME, st, sizeof st, &need)) std::strcpy(st, "WinSta0");
        std::string n = "gmod-mcp-" + std::to_string(GetCurrentProcessId());
        std::wstring wn = utf8_to_wide(n);
        HDESK d = CreateDesktopW(wn.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (!d) {
            err = "CreateDesktop failed (error " + std::to_string(GetLastError()) + ")";
            return false;
        }
        g_desk = d;
        g_desk_name = n;
        g_desk_station = st;
    }
    name = g_desk_name;
    station = g_desk_station;
    return true;
}

// Closes the desktop when no client started on it runs any more (and no launch is about to use it). The processes that still run on it
// would keep it alive anyway: closing the handle is harmless, the next launch makes a new one.
void release_private_desktop_if_unused() {
    if (g_desk_launching.load() > 0) return;
    {
        std::lock_guard<std::mutex> lk(g_launch_mu);
        for (const auto &l : g_launched)
            if (l.private_desktop && l.h && WaitForSingleObject(l.h, 0) == WAIT_TIMEOUT) return;
    }
    std::lock_guard<std::mutex> lk(g_desk_mu);
    if (g_desk && g_desk_launching.load() == 0) {
        CloseDesktop(g_desk);
        g_desk = nullptr;
    }
}

// Session end (the clients were just terminated): drop the handle whatever runs.
void close_private_desktop() {
    std::lock_guard<std::mutex> lk(g_desk_mu);
    if (g_desk) {
        CloseDesktop(g_desk);
        g_desk = nullptr;
    }
}
#else
inline void release_private_desktop_if_unused() {}
inline void close_private_desktop() {}
#endif

json launch_tool(const json &args, const json &token) {
#ifndef _WIN32
    (void)args;
    (void)token;
    return tool_error("unsupported", "instances_launch is Windows only for now: on Linux start the client with steam -applaunch 4000 -mcp -multirun ...");
#else
    int count = args.value("count", 1);
    std::string preset = args.value("preset", "light");
    // Issue #14: by default the clients run on a private desktop of this session; there nobody sees them, so they are silent unless asked.
    bool private_desk = args.value("desktop", "private") != "shared";
    bool sound = args.value("sound", !private_desk), keep = args.value("keep", false);
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
        std::string code, inside = lua_mount::refuse_mount(mount_root, root + "/garrysmod", args.value("allow_shadow", false), &code);
        if (!inside.empty()) return tool_error(code, "lua_mount: " + inside);
    }
    // Issue #13: the module installed in this game folder against the one this bridge ships, before anything starts (a client of an older module
    // starts, answers, and only then says "version mismatch": the launch, its settle delay and its map load are wasted).
    ModuleCheck module_chk = check_installed_module(root);
    if (module_chk.verdict.outdated() && !args.value("ignore_outdated_module", false)) {
        std::string had = module_chk.verdict.installed.empty() ? "an older release (it carries no version marker)" : module_chk.verdict.installed;
        json err{{"code", "module_outdated"},
                 {"message", "instances_launch refused: the gmod-mcp module installed in the game (" + had + ", " + module_chk.installed_file +
                                 ") is older than the one this plugin ships (" + module_chk.verdict.shipped + "). The clients would start with the old module and report a "
                                 "version mismatch only afterwards. Nothing was started. " + module_update_steps(module_chk, root) +
                                 " Or pass ignore_outdated_module: true to start anyway."},
                 {"details", {{"installed", module_chk.verdict.installed.empty() ? json(nullptr) : json(module_chk.verdict.installed)},
                              {"shipped", module_chk.verdict.shipped},
                              {"installed_file", module_chk.installed_file},
                              {"shipped_file", module_chk.shipped_file}}}};
        return tool_text(json{{"error", err}}, true);
    }
    json peers_list = game_call("instances_list", json::object(), 3);  // an unreachable game is an error object: no ids
    // Issue #5 (live verification of 0.3.7): a program that is not gmod-mcp holds the base port. The module takes the next free port, which
    // this bridge never looks at (every call goes through the base port): the clients would start, never be seen and the launch would fail
    // `timeout` after its whole budget. Refused up front, nothing started, with the way out.
    if (g_conflict && peers_list.is_object() && peers_list.value("error", json::object()).value("code", "") == "port_conflict") {
        for (int i = 0; i < 25 && !g_found_port; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));  // the probe of the next ports runs in the background
        return tool_error("port_conflict", "instances_launch refused: " + port_conflict_text() + " Nothing was started: a client launched now would register with whatever answers on that port, "
                                                          "and this bridge could not see it.");
    }
    // Issue #8 (chaos test G1): a frozen client blocks a new one from registering: refuse up front, with the way out.
    if (!args.value("ignore_unhealthy", false)) {
        json bad = unhealthy_peers(peers_list);
        if (!bad.empty()) {
            std::string pids;
            for (const auto &u : bad) pids += (pids.empty() ? "" : ", ") + std::to_string(u.value("pid", 0u));
            return tool_text(json{{"error", {{"code", "peer_unhealthy"},
                                            {"message", "a running client is stuck: " + describe_unhealthy(bad) + ". A client frozen as a whole (or mid-load) keeps a new client from "
                                                        "registering (engine FP7137 handshake): the launch would time out with `0 of N clients answered`. Close it first "
                                                        "(instances_close {pids: [" + pids + "]} for a client this session started, or game_terminate {instance}), or pass "
                                                        "ignore_unhealthy: true to start anyway. Nothing was started."},
                                            {"details", {{"unhealthy", bad}}}}}},
                             true);
        }
    }
    // Issue #8: memory first. A machine out of commit crashes every game on it (11 clients, 1.8 GB free: all died, no dump).
    {
        lowmem::Memory mem = lowmem::current();
        if (mem.known && !args.value("ignore_low_memory", false)) {
            std::set<uint32_t> alive;
            for (const auto &e : peers_list.value("instances", json::array()))
                if (e.is_object() && e.value("state", "") != "crashed" && e.value("mode", "client") != "srcds") alive.insert(e.value("pid", 0u));
            {
                std::lock_guard<std::mutex> lk(g_launch_mu);
                for (const auto &l : g_launched)
                    if (l.h && WaitForSingleObject(l.h, 0) == WAIT_TIMEOUT) alive.insert(l.pid);
            }
            alive.erase(0u);
            // Issue #8 (chaos test G4): clients other sessions started that have not registered yet are not in the directory: count the
            // game's processes too (the union by pid, so a registered one is not counted twice), and report both numbers.
            int registered = (int)alive.size();
            std::string gexe = gamedir::find_game_exe(root);
            std::set<uint32_t> procs = gexe.empty() ? std::set<uint32_t>() : game_process_pids(gexe);
            alive.insert(procs.begin(), procs.end());
            lowmem::Verdict lv = lowmem::check_launch(mem, count, (int)alive.size(), false, registered, (int)procs.size());
            if (lv.refuse) return tool_text(json{{"error", {{"code", "low_memory"}, {"message", lv.message}, {"details", lv.details}}}}, true);
        }
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
    if (!mount_root.empty()) opts += " -mcp_lua_mount \"" + mount_root + "\"" + (args.value("allow_shadow", false) ? " -mcp_lua_mount_allow_shadow" : "");
    // Issue #10: the extra tokens (validated: no space, quote or ';'), after ours so that a `+convar` of theirs wins; then the
    // names of the convars they and the commands may change, which the module gives back to the user's values before a
    // clean quit (an older module ignores this option).
    for (const auto &t : extra_args) opts += " " + t;
    // Issue #8 (chaos test G3): the owner label rides on the command line (encoded: no space, quote or ';') and in the module's
    // heartbeat, so instances_list names it and a later bridge session can close this client by owner even with keep: true.
    if (!owner.empty()) opts += " -mcp_owner " + launchopts::encode_owner(owner);
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
    if (!fp_gate.ok()) return retry_later(fp_gate.error_result(), "instances_launch", args, 180.0);  // FP7137
    if (fp_gate.waited_s() > 0) g_fp7137_progress_offset = fp_gate.waited_s() + 1;  // FP7137
    std::vector<Launched> started;
    unsigned long long return_target = 0;  // the foreground window before the first start: the clients give the foreground back to it
    // Issue #14: the private desktop (created now, kept while a client runs on it). The count keeps release_private_desktop_if_unused away
    // until these clients are in g_launched.
    std::string desk_name, desk_station;
    struct DeskGuard {
        std::atomic<int> &n;
        explicit DeskGuard(std::atomic<int> &c) : n(c) { ++n; }
        ~DeskGuard() { --n; }
    } desk_guard(g_desk_launching);
    if (private_desk) {
        std::string derr;
        if (!private_desktop(desk_name, desk_station, derr)) {
            fp_gate.release();
            return tool_error("launch_failed", "could not create the private desktop for the clients: " + derr + ". Nothing was started. Pass desktop: \"shared\" to start them on the user's desktop.");
        }
    }
    std::wstring wdesk = utf8_to_wide(desk_station + "\\" + desk_name);
    for (int i = 0; i < count; ++i) {
        // A test client takes the foreground window when it starts: tell it which window had it, so it hands it back once its own window is up.
        // The foreground window of a client this launch just started (it has not handed it back yet) is not a target: keep the earlier one.
        std::string client_opts = opts;
        if (!private_desk) {  // on the private desktop the client never takes the user's foreground: nothing to give back (issue #14)
            plat::Foreground fg;
            bool have = plat::foreground_window(fg);
            bool ours = false;
            for (const auto &l : started) ours = ours || (have && l.pid == fg.pid);
            return_target = launchopts::pick_return_target(have ? fg.hwnd : 0, ours, return_target);
            std::string t = launchopts::format_return_focus(return_target);
            if (!t.empty()) client_opts += " -mcp_return_focus " + t;
        }
        std::string cmd = "\"" + exe + "\" " + client_opts;
        std::wstring wcmd = utf8_to_wide(cmd), wdir = utf8_to_wide(root);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESHOWWINDOW;  // the first ShowWindow of the client shows its window without activating it (it does not take the foreground)
        si.wShowWindow = SW_SHOWNOACTIVATE;
        if (private_desk) si.lpDesktop = wdesk.data();  // issue #14
        PROCESS_INFORMATION pi{};
        HANDLE job = keep ? nullptr : client_job();  // issue #8: ends with the bridge unless keep: true
        if (!CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE, job ? CREATE_SUSPENDED : 0, nullptr, wdir.c_str(), &si, &pi)) {
            DWORD cpe = GetLastError();
            for (auto &l : started) TerminateProcess(l.h, 0);
            fp_gate.release();
            return tool_error("launch_failed", "CreateProcess failed (error " + std::to_string(cpe) + ") for " + exe +
                                                   (private_desk ? " on the private desktop " + desk_name + ": pass desktop: \"shared\" to start the clients on the user's desktop" : ""));
        }
        if (job) {
            AssignProcessToJobObject(job, pi.hProcess);  // a failure (a parent job that forbids nesting): the client just runs without the guard
            ResumeThread(pi.hThread);
        }
        CloseHandle(pi.hThread);
        fp_gate.note_started();  // FP7137
        Launched l;
        l.pid = pi.dwProcessId;
        l.h = pi.hProcess;
        l.keep = keep;
        l.private_desktop = private_desk;
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
    std::map<std::string, json> seen_unhealthy;  // issue #8 (G1): peers that were stuck while this launch waited
    // Issue #5: the base port taken by a program that is not gmod-mcp while the clients start: they cannot be seen (the module took the next
    // port), so waiting the whole budget is pointless: gives up after kConflictGiveUpS of conflict and names it in the timeout error.
    const double kConflictGiveUpS = 10.0;
    double conflict_since = -1;
    while ((int)found.size() < count && elapsed() < timeout && !(conflict_since >= 0 && elapsed() - conflict_since > kConflictGiveUpS)) {
        json lst = game_call("instances_list", json::object(), 5);
        if (g_conflict) {
            if (conflict_since < 0) conflict_since = elapsed();
        } else {
            conflict_since = -1;
        }
        note_instances(lst);
        for (const auto &u : unhealthy_peers(lst)) seen_unhealthy[u.value("instance", std::string("?"))] = u;
        for (const auto &in : lst.value("instances", json::array()))
            for (auto &l : started)
                if (in.value("pid", 0u) == l.pid && in.value("state", "") != "crashed" && !found.contains(std::to_string(l.pid))) {
                    found[std::to_string(l.pid)] = in;
                    l.id = in.value("id", "");
                    progress(token, (double)found.size(), count, l.id + " answers");
                }
        for (auto &l : started)
            if (WaitForSingleObject(l.h, 0) == WAIT_OBJECT_0 && !found.contains(std::to_string(l.pid))) {
                // Issue #8 (chaos test G2): the siblings of this call that still run are not closed: name them, so the caller does not leak them.
                json alive_sibs = json::array();
                std::string sib_text;
                for (auto &o : started) {
                    if (&o == &l || WaitForSingleObject(o.h, 0) != WAIT_TIMEOUT) continue;
                    bool answered = found.contains(std::to_string(o.pid));
                    json x{{"pid", o.pid}, {"state", answered ? "answering" : "still starting"}};
                    std::string iid = answered ? found[std::to_string(o.pid)].value("id", "") : o.id;
                    if (!iid.empty()) x["instance"] = iid;
                    alive_sibs.push_back(x);
                    sib_text += (sib_text.empty() ? "" : ", ") + std::string("pid ") + std::to_string(o.pid) + (iid.empty() ? "" : " (" + iid + ")") + " " + x["state"].get<std::string>();
                }
                json ce = launched_crash_error(l);
                if (ce.is_null())
                    ce = json{{"code", "launch_failed"}, {"message", "client pid " + std::to_string(l.pid) + " exited during startup (Steam not running? see garrysmod/console.log)"}};
                json res_err{{"error", ce}};
                if (!alive_sibs.empty()) {
                    if (!ce.contains("details") || !ce["details"].is_object()) ce["details"] = json::object();
                    ce["details"]["still_running"] = alive_sibs;
                    res_err["error"] = ce;
                    res_err["warning"] = "the other client(s) of this call are NOT closed and keep running: " + sib_text +
                                         ". Close them with instances_close {pids: [...]}" + (owner.empty() ? "" : " or instances_close {owner: \"" + owner + "\"}") +
                                         " (or use them), or they leak.";
                }
                return tool_text(res_err, true);
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
        json stuck = json::array();
        for (auto &kv : seen_unhealthy) stuck.push_back(kv.second);
        std::string stuck_note = stuck.empty() ? "" : " A running client was stuck meanwhile: " + describe_unhealthy(stuck) +
                                                          " (a frozen client keeps a new one from registering, engine FP7137 handshake): close it with instances_close {pids} or game_terminate {instance}.";
        json terr{{"code", "timeout"},
                  {"message", std::to_string(found.size()) + " of " + std::to_string(count) +
                                  " clients answered within " + std::to_string((int)std::min(timeout, elapsed() + 0.5)) +
                                  " s (still starting, or gmod-mcp did not load in them: see "
                                  "garrysmod/data/mcpagent/last_crash.txt and the console). Close "
                                  "the silent ones with instances_close {pids}." + stuck_note +
                                  (g_conflict ? " port_conflict: " + port_conflict_text() + " The silent clients may be running fine on another port; this bridge cannot see them until the port is sorted out." : "")}};
        if (!stuck.empty()) terr["details"] = json{{"unhealthy_peers", stuck}};
        return tool_text(json{{"error", terr},
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
    json mount_fix;  // next_call for the first client whose mount is wrong (issue #3)
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
    // Maps: at most 2 at a time (per call here, and across every bridge through the gate), after the settle delay (FP7137: was all at once).
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
                              code == "coordinator_changing" || code == "relay_error" || code == "rpc_error" || code == "relay_lost";
                json ce = launched_crash_error(started[i], broken ? 2500 : 0);
                if (!ce.is_null()) {
                    map_res[i] = json{{"error", ce}};
                } else if (broken && !started[i].id.empty()) {
                    // Issue #6: the answer was lost (the coordinator moved while the load ran), the load may have gone through: ask the client.
                    json ok = recover_lost_map_load(started[i], map, std::max(10.0, timeout - elapsed()));
                    if (!ok.is_null()) map_res[i] = ok;
                }
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
            if (!v.value("active", false)) {
                if (mount_ok[i]) mount_problems.push_back(started[i].id + ": " + v.value("error", std::string("not active after the map load")));
                if (mount_fix.is_null()) mount_fix = next_call::make("lua_mount", json{{"path", mount_root}, {"instance", started[i].id}});
            } else if (v.value("base_lua_mounted", json()) == json(false)) {
                mount_problems.push_back(started[i].id + ": the game's own lua/ folder is missing from its server Lua paths (base Lua not mounted)");
                if (mount_fix.is_null()) mount_fix = next_call::make("lua_mount", json{{"path", nullptr}, {"instance", started[i].id}});  // remove it
            }
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
            if (ok && map_res[i].contains("note")) e["map_note"] = map_res[i]["note"];  // issue #6: the answer was lost, the client has the map
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
    // Issue #14: where the clients run, and what that changes for the caller.
    res["desktop"] = private_desk ? "private" : "shared";
    res["sound"] = sound;
    if (private_desk) {
        res["desktop_name"] = desk_name;
        res["desktop_note"] =
            "the clients run on a private Windows desktop (" + desk_name + "): no window on the user's screen, no taskbar button, nothing to close by hand (instances_close), "
            "and they never take the user's focus (no -mcp_return_focus). The user's own game is not disturbed by their start (no GPU release/restore, no freeze). "
            "ui_click, ui_key and screenshot work as usual (posted window messages and the engine's own capture). " +
            std::string(sound ? "Sound is on (sound: true): the clients are audible although nobody sees them. " : "Sound is off by default there (-nosound): sound: true turns it on (SoundDuration of ADPCM WAV files differs without sound). ") +
            "The clients of this session share the desktop, so a client that starts still disturbs the others of the session: launch them in one call. desktop: \"shared\" starts them on the user's desktop.";
    }
    // Issue #13: what the module check found (an outdated module only gets here with ignore_outdated_module).
    if (module_chk.verdict.state != modcheck::State::Unknown) {
        json m = module_json(module_chk);
        res["module"] = m;
        if (module_chk.verdict.outdated()) {
            std::string w = "the gmod-mcp module installed in the game (" + (module_chk.verdict.installed.empty() ? std::string("an older release") : module_chk.verdict.installed) +
                            ") is older than the one this plugin ships (" + module_chk.verdict.shipped + "): started anyway (ignore_outdated_module). " + module_update_steps(module_chk, root);
            res["warning"] = res.contains("warning") ? json(res["warning"].get<std::string>() + " " + w) : json(w);
        } else if (module_chk.verdict.state == modcheck::State::Missing) {
            std::string w = "no module file in " + module_chk.installed_file + ": the clients only answer when an addon carries the module. The shipped one is " + module_chk.shipped_file + ".";
            res["warning"] = res.contains("warning") ? json(res["warning"].get<std::string>() + " " + w) : json(w);
        }
    }
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
    if (!mount_fix.is_null()) res["next_call"] = mount_fix;
    if (!all_ok) {
        // Issue #3 (Code Mode): an isError result always carries {"error": {"code", "message"}} (Claude Code throws the text as
        // e.detail). Here the clients did start: the instances stay in the result, and the error says what did not work.
        std::string why;
        auto add = [&why](const std::string &t) { why += (why.empty() ? "" : "; ") + t; };
        for (const auto &mp : mount_problems) add(mp.get<std::string>());
        if (!commands_ok) add("a startup command failed (instances[].commands)");
        for (const auto &e : results)
            if (e.contains("map_error")) {
                const json &me = e["map_error"];
                add(e.value("instance", std::string("a client")) + ": map " + map + " not loaded" +
                    (me.is_object() && me.contains("code") ? ": " + me.value("code", std::string()) : ""));
            }
        res["error"] = json{{"code", "launch_incomplete"},
                            {"message", "the clients were started (see instances), but not everything you asked for worked: " + (why.empty() ? std::string("see instances[]") : why)}};
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
    bool owner_local_match = false;
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
                owner_local_match = any;
            }
        }
    }
    // Issue #8 (chaos test G3): keep:true test clients of an earlier bridge session carry the owner in their heartbeat (-mcp_owner):
    // those that are test clients (-mcp_test_client) and carry exactly this tag are closed too. Never a game that is not a test
    // client (the user's own), never one without the tag.
    std::vector<json> foreign;
    std::string not_test_clients;
    if (!owner.empty() && !all) {
        json lst = game_call("instances_list", json::object(), 5);
        std::set<uint32_t> mine;
        for (const auto &l : todo) mine.insert(l.pid);
        {
            std::lock_guard<std::mutex> lk(g_launch_mu);
            for (const auto &l : g_launched) mine.insert(l.pid);  // another owner's client of this session: not ours to take here
        }
        for (const auto &e : lst.value("instances", json::array())) {
            if (!e.is_object() || e.value("owner", "") != owner || mine.count(e.value("pid", 0u)) || e.value("pid", 0u) == 0) continue;
            std::string st = e.value("state", "running");
            if (st == "crashed" || e.value("mode", "client") == "srcds") continue;
            if (e.value("test_client", false) != true) {
                refused.push_back(e.value("id", "?"));
                not_test_clients += (not_test_clients.empty() ? "" : ", ") + e.value("id", std::string("?"));
                continue;
            }
            foreign.push_back(e);
        }
        if (!owner_local_match && foreign.empty() && not_test_clients.empty()) refused.push_back("owner:" + owner);
    } else if (!owner.empty() && !owner_local_match) {
        refused.push_back("owner:" + owner);
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
    json already_gone = json::array();  // issue #8: processes that had ended before this call: nothing was closed
    for (auto &l : todo) {
        bool gone = false;
#ifdef _WIN32
        gone = l.h && WaitForSingleObject(l.h, 0) == WAIT_OBJECT_0;
#endif
        if (!gone && !l.id.empty()) game_call("game_terminate", json{{"instance", l.id}}, 5);
#ifdef _WIN32
        if (!gone && WaitForSingleObject(l.h, 3000) != WAIT_OBJECT_0) TerminateProcess(l.h, 0);
        CloseHandle(l.h);
#endif
        json c{{"pid", l.pid}};
        if (!l.id.empty()) c["instance"] = l.id;
        if (!l.owner.empty()) c["owner"] = l.owner;
        if (gone) {
            already_gone.push_back(c);
            std::lock_guard<std::mutex> lk(g_default_mu);
            if (g_default_instance == l.id) g_default_instance.clear();
            continue;
        }
        if (!l.id.empty() && l.id == coordinator_before && others_alive > 0) {
            c["was_coordinator"] = true;
            coordinator_ended = true;
        }
        closed.push_back(c);
        std::lock_guard<std::mutex> lk(g_default_mu);
        if (g_default_instance == l.id) g_default_instance.clear();
    }
    for (const auto &e : foreign) {
        std::string id = e.value("id", "");
        uint32_t pid = e.value("pid", 0u);
        bool gone = false;
#ifdef _WIN32
        HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
        gone = !h || WaitForSingleObject(h, 0) == WAIT_OBJECT_0;  // issue #8: the list still shows it (a game that just quit)
        if (gone) {
            if (h) CloseHandle(h);
            h = nullptr;
        }
#endif
        if (!gone) game_call("game_terminate", json{{"instance", id}}, 5);
#ifdef _WIN32
        if (h) {
            if (WaitForSingleObject(h, 3000) != WAIT_OBJECT_0) TerminateProcess(h, 0);
            CloseHandle(h);
        }
#endif
        (gone ? already_gone : closed).push_back(json{{"pid", pid}, {"instance", id}, {"owner", owner}, {"launched_by_other_session", true}});
        std::lock_guard<std::mutex> lk(g_default_mu);
        if (g_default_instance == id) g_default_instance.clear();
    }
    release_private_desktop_if_unused();  // issue #14: the last client of the session on the private desktop is gone
    json res{{"closed", closed}};
    if (!already_gone.empty()) {
        res["already_gone"] = already_gone;
        res["note"] = "already_gone: the process had ended before this call (it quit or crashed by itself): nothing was closed for it";
    }
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
        if (!not_test_clients.empty())
            res["why_refused"] = res["why_refused"].get<std::string>() + ". " + not_test_clients + (owner.empty() ? "" : " carries owner \"" + owner + "\"") +
                                 " but is not a test client (not started by instances_launch): never closed by owner";
    }
    bool nothing_closed = !refused.empty() && closed.empty();
    // Issue #3 (Code Mode): an error result always carries {"error": {"code", "message"}} (the text of an isError result is
    // thrown by Claude Code as e.detail); the other fields stay for the callers that read them.
    if (nothing_closed) res["error"] = json{{"code", "refused"}, {"message", res.value("why_refused", std::string("nothing was closed"))}};
    return tool_text(res, nothing_closed);
}

json select_tool(const json &args) {
    const json &v = args.contains("instance") ? args["instance"] : json();
    std::string want = v.is_string() ? v.get<std::string>() : "";
    std::string unchecked;  // why the id could not be checked
    if (!want.empty()) {
        // Issue #3: an id the directory does not list used to be accepted, and every later call then failed with
        // unknown_instance. Checked before taking the lock (game_call talks to the game). A game this session just
        // started may not have registered yet: its id is known from the launch.
        bool launched = false;
        {
            std::lock_guard<std::mutex> lk(g_launch_mu);
            for (const auto &l : g_launched) launched = launched || l.id == want;
        }
        json lst = launched ? json::object() : game_call("instances_list", json::object(), 3);
        if (!launched) {
            if (lst.is_object() && lst.contains("instances") && lst["instances"].is_array()) {
                json known = json::array();
                bool found = false;
                for (const auto &in : lst["instances"])
                    if (in.is_object() && in.contains("id") && in["id"].is_string()) {
                        known.push_back(in["id"]);
                        found = found || in["id"] == want;
                    }
                if (!found) {
                    json err = tool_error("unknown_instance", "no instance '" + want + "' (known: " + (known.empty() ? std::string("none") : known.dump()) +
                                                              "): the default was not changed. instances_list shows the ids.");
                    err["structuredContent"]["error"]["details"] = json{{"instance", want}, {"known", known}};
                    err["content"][0]["text"] = err["structuredContent"].dump();
                    return err;
                }
            } else {
                unchecked = "the id was not checked: the list of instances could not be read (the game may be unreachable); an unknown id makes later calls fail with unknown_instance";
            }
        }
    }
    std::lock_guard<std::mutex> lk(g_default_mu);
    std::string prev = g_default_instance;
    g_default_instance = want;
    json out{{"default_instance", g_default_instance.empty() ? json(nullptr) : json(g_default_instance)},
             {"previous", prev.empty() ? json(nullptr) : json(prev)}};
    if (!unchecked.empty()) out["warning"] = unchecked;
    return tool_text(out, false);
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
    close_private_desktop();  // issue #14
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
    if (method == "tools/call") strip_tasks_extension(fwd);  // Issue #3: GMOD_MCP_TASKS=off
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
        if (!fp_gate.aborted()) out(local_result(msg, retry_later(fp_gate.error_result(), tool, params_of(fwd).value("arguments", json::object()), 180.0)));  // FP7137
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
        if (!tool.empty()) {  // issue #6: a call relayed to another instance was answered: several games live
            // Only a relayed answer counts (`relayed_by`, set by the coordinator when the target is another game). A call
            // that names the coordinator itself is not a sign of other games: counting it made every call wait 10 s for a
            // hand-over after the only game closed (agents always pass `instance`).
            const json &fa = params_of(fwd).value("arguments", json::object());
            const json &res = r.response.contains("result") ? r.response["result"] : json();
            const json &sc = res.is_object() && res.contains("structuredContent") ? res["structuredContent"] : json();
            if (fa.is_object() && fa.contains("instance") && fa["instance"].is_string() && res.is_object() &&
                !res.value("isError", false) && sc.is_object() && sc.contains("relayed_by"))
                g_multi_seen_ms = steady_ms();
        }
        if (tool == "instances_list" && r.response.contains("result") && !r.response["result"].value("isError", false)) {
            json &res = r.response["result"];
            res = tool_text(annotate_instances(res.value("structuredContent", json::object()), params_of(msg).value("arguments", json::object())), false);
        }
        if (!tool.empty() && r.response.contains("result")) add_lua_error_warnings(r.response["result"], tool);  // issue #15
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
        sc.update(offline_module_info());  // issue #13: module_outdated / module_check
        out(local_result(msg, tool_text(annotate_instances(sc, params_of(msg).value("arguments", json::object()), false), false)));  // the bridge's own empty list: not a reading of the games
    } else if (method == "tools/call" && !launched_answer(fwd).is_null()) {
        out(local_result(msg, launched_answer(fwd)));
    } else if (method == "tools/call") {
        out(local_result(msg, not_running_error()));
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

#include "console.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <regex>
#include <thread>

#include "log.h"
#include "platform/platform.h"

using nlohmann::json;

namespace console {

// --- tier0 types (Source SDK 2013, dbg.h) ---
enum SpewType_t { SPEW_MESSAGE = 0, SPEW_WARNING, SPEW_ASSERT, SPEW_ERROR, SPEW_LOG, SPEW_TYPE_COUNT };
enum SpewRetval_t { SPEW_DEBUGGER = 0, SPEW_CONTINUE, SPEW_ABORT };
using SpewOutputFunc_t = SpewRetval_t (*)(SpewType_t, const char *);
using SetSpewFn = void (*)(SpewOutputFunc_t);
using GetSpewFn = SpewOutputFunc_t (*)();

static const auto g_start = std::chrono::steady_clock::now();
double uptime() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count(); }

// --- Console ring buffer ---
static std::mutex g_mu;
static std::deque<Line> g_lines;
static size_t g_capacity = 20000;
static uint64_t g_next_seq = 0;  // sequence number of the next line
static std::string g_partial[SPEW_TYPE_COUNT];
static std::string g_method = "none";

// --- Errors ---
static std::mutex g_emu;
static std::deque<json> g_errors;
static const size_t kErrCapacity = 2000;
static uint64_t g_err_next = 0;
static uint64_t g_err_floor = 0;  // sequences < floor were cleared on purpose (errors_clear): not losses
static bool g_parse_errors = false;
static uint64_t (*g_map_load_fn)() = nullptr;  // constant-initialized: safe to set from another file's static init
// Error parser state (srcds mode).
static json g_pending_err;

static void flush_pending_error_locked() {
    if (g_pending_err.is_null()) return;
    std::lock_guard<std::mutex> lk(g_emu);
    g_pending_err["seq"] = g_err_next++;
    g_errors.push_back(g_pending_err);
    while (g_errors.size() > kErrCapacity) g_errors.pop_front();
    g_pending_err = nullptr;
}

// Non-throwing integer parse for numbers read from console text (std::stoi throws on overflow: it would unwind into the
// engine's print callback). Fails on anything that does not fit comfortably in an int.
static bool parse_small_int(const std::string &s, int &out) {
    size_t i = 0;
    bool neg = false;
    if (i < s.size() && s[i] == '-') {
        neg = true;
        ++i;
    }
    if (i >= s.size() || s.size() - i > 9) return false;
    long v = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
    }
    out = (int)(neg ? -v : v);
    return true;
}

static void parse_error_line_impl(const std::string &text) {
    // A real error line is short; never run a regex on a huge one (cost, and std::regex recursion depth).
    if (text.size() > 4096) {
        flush_pending_error_locked();
        return;
    }
    // GMod formats: "[ERROR] lua/x.lua:3: msg" then "  1. fn - lua/x.lua:3"; or "[addon] lua/x.lua:3: msg".
    // [ERROR] lines: any source with a line number (chunks without a file: "mcpagent:lua_exec:1: msg"). Lines tagged
    // with an addon name: a .lua file only, so ordinary "[tag] text" messages are not taken for errors.
    static const std::regex head_err(R"(^\[ERROR\] (\S+:\d+: .*)$)");
    static const std::regex head(R"(^\[([^\]]+)\] (.*\.lua:\d+: .*)$)");
    static const std::regex frame(R"(^\s+(\d+)\. (\S*) - (.*):(-?\d+)\s*$)");
    std::smatch m;
    std::string message, addon;
    bool is_head = false;
    if (std::regex_match(text, m, head_err)) {
        message = m[1].str();
        is_head = true;
    } else if (std::regex_match(text, m, head)) {
        message = m[2].str();
        addon = m[1].str() == "ERROR" ? "" : m[1].str();
        is_head = true;
    }
    if (is_head) {
        flush_pending_error_locked();
        g_pending_err = json{{"realm", "server"}, {"message", message}, {"addon", addon}, {"stack", json::array()},
                             {"time", uptime()}, {"source", "console"}, {"map_load", g_map_load_fn ? g_map_load_fn() : 0}};
    } else if (!g_pending_err.is_null() && std::regex_match(text, m, frame)) {
        int level = 0, line = 0;
        if (parse_small_int(m[1].str(), level) && parse_small_int(m[4].str(), line))
            g_pending_err["stack"].push_back(json{{"level", level}, {"function", m[2].str()}, {"file", m[3].str()}, {"line", line}});
        // else: a garbage frame (numbers out of range): ignored, the error stays open
    } else {
        flush_pending_error_locked();
    }
}

// Called from the engine's print path: nothing may escape (std::regex can throw regex_error, json and string can throw
// bad_alloc, ...). A failure only loses the error being assembled.
static void parse_error_line(const std::string &text) {
    try {
        parse_error_line_impl(text);
    } catch (...) {
        try {
            g_pending_err = nullptr;
        } catch (...) {
        }
    }
}

static void push_line_locked(const std::string &level, const std::string &text) {
    g_lines.push_back(Line{g_next_seq++, uptime(), level, text});
    while (g_lines.size() > g_capacity) g_lines.pop_front();
}

void push_line(const std::string &level, const std::string &text) {
    std::lock_guard<std::mutex> lk(g_mu);
    push_line_locked(level, text);
    if (g_parse_errors) parse_error_line(text);
}

static const char *level_name(int t) {
    switch (t) {
        case SPEW_MESSAGE: return "message";
        case SPEW_WARNING: return "warning";
        case SPEW_ASSERT: return "assert";
        case SPEW_ERROR: return "error";
        case SPEW_LOG: return "log";
        default: return "message";
    }
}

static LineObserver g_observer;

void set_line_observer(LineObserver fn) { g_observer = std::move(fn); }

static void feed_impl(int type, const char *msg) {
    if (!msg) return;
    if (type < 0 || type >= SPEW_TYPE_COUNT) type = SPEW_MESSAGE;
    std::vector<std::string> done;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        std::string &buf = g_partial[type];
        buf += msg;
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            push_line_locked(level_name(type), line);
            if (g_parse_errors) parse_error_line(line);
            done.push_back(std::move(line));
            buf.erase(0, pos + 1);
        }
        if (buf.size() > 65536) {  // endless line: cut it
            push_line_locked(level_name(type), buf);
            buf.clear();
        }
    }
    // Observer is called outside the console lock (it may publish events).
    if (g_observer)
        for (const auto &l : done) {
            try {
                g_observer(l);  // a throwing observer must not stop the remaining lines
            } catch (...) {
            }
        }
}

// Entry point of every engine print (spew hook, logging listener, console.log tail): never throws into the engine.
static void feed(int type, const char *msg) {
    try {
        feed_impl(type, msg);
    } catch (...) {
    }
}

// --- Spew hook ---
static SetSpewFn g_set_spew = nullptr;
static GetSpewFn g_get_spew = nullptr;
static std::atomic<SpewOutputFunc_t> g_prev{nullptr};
static thread_local int t_depth = 0;

static SpewRetval_t our_spew(SpewType_t type, const char *msg) {
    if (t_depth > 0) return SPEW_CONTINUE;  // re-entrancy (circular chain): cut it
    ++t_depth;
    try {
        feed(type, msg);
    } catch (...) {  // never throw into the engine (feed already guards itself; belt and braces)
    }
    SpewOutputFunc_t prev = g_prev.load();
    SpewRetval_t r = prev ? prev(type, msg) : SPEW_CONTINUE;
    --t_depth;
    return r;
}

// --- Logging system (newer tier0, e.g. the x86-64 Linux dedicated server: no SpewOutputFunc export) ---
// Layout of the public logging API (Alien Swarm SDK, tier0/logging.h): a listener is an object whose first virtual
// function is Log(const LoggingContext_t *, const char *). Registered next to the engine's own listeners (console,
// log file), so the game's output is unchanged.
struct LoggingContext {
    int channel;
    int flags;
    int severity;  // 0 message, 1 warning, 2 assert, 3 error
    unsigned char color[4];
};
class LoggingListener {
public:
    virtual void Log(const LoggingContext *ctx, const char *msg) {
        if (t_log_depth > 0) return;
        ++t_log_depth;
        try {
            int sev = ctx ? ctx->severity : 0;
            feed(sev >= 0 && sev <= 3 ? sev : SPEW_MESSAGE, msg);
        } catch (...) {  // never throw into the engine
        }
        --t_log_depth;
    }
    static thread_local int t_log_depth;
};
thread_local int LoggingListener::t_log_depth = 0;
static LoggingListener g_listener;
using ListenerFn = void (*)(LoggingListener *);
static ListenerFn g_unregister_listener = nullptr;

// --- -condebug fallback: incremental reading of console.log ---
static std::atomic<bool> g_tail_run{false};
static std::thread g_tail_thread;

static void tail_loop(std::string path) {
    size_t offset = 0;
    {
        std::string all;
        if (plat::read_file(path, all)) offset = all.size();  // do not re-read the history
    }
    std::string partial;
    while (g_tail_run) {
        std::string all;
        if (plat::read_file(path, all)) {
            if (all.size() < offset) offset = 0;  // file truncated
            if (all.size() > offset) {
                feed(SPEW_MESSAGE, all.substr(offset).c_str());
                offset = all.size();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

std::string install(const std::string &condebug_path) {
    void *t0 = plat::find_loaded_module(plat::tier0_names());
    g_set_spew = reinterpret_cast<SetSpewFn>(plat::find_symbol(t0, "SpewOutputFunc"));
    g_get_spew = reinterpret_cast<GetSpewFn>(plat::find_symbol(t0, "GetSpewOutputFunc"));
    if (g_set_spew && g_get_spew) {
        SpewOutputFunc_t cur = g_get_spew();
        if (cur != our_spew) {
            g_prev = cur;
            g_set_spew(our_spew);
        }
        g_method = "spew";
        return g_method;
    }
    auto reg = reinterpret_cast<ListenerFn>(plat::find_symbol(t0, "LoggingSystem_RegisterLoggingListener"));
    g_unregister_listener = reinterpret_cast<ListenerFn>(plat::find_symbol(t0, "LoggingSystem_UnregisterLoggingListener"));
    if (reg && g_unregister_listener) {
        reg(&g_listener);
        g_method = "logging";
        return g_method;
    }
    if (!condebug_path.empty()) {
        g_tail_run = true;
        g_tail_thread = std::thread(tail_loop, condebug_path);
        g_method = "condebug";
        return g_method;
    }
    g_method = "none";
    return g_method;
}

void maintain() {
    if (g_method != "spew") return;
    SpewOutputFunc_t cur = g_get_spew();
    if (cur != our_spew) {
        // Someone replaced the spew function: put ourselves back in front, chaining the new one.
        g_prev = cur;
        g_set_spew(our_spew);
        logx::warn("spew function replaced by a third party, capture reinstalled");
    }
}

void uninstall() {
    if (g_method == "spew" && g_get_spew && g_get_spew() == our_spew) g_set_spew(g_prev.load());
    if (g_method == "logging" && g_unregister_listener) g_unregister_listener(&g_listener);
    if (g_tail_run) {
        g_tail_run = false;
        if (g_tail_thread.joinable()) g_tail_thread.join();
    }
    g_method = "none";
}

std::string method() { return g_method; }

uint64_t cursor_now() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_next_seq;
}

size_t capacity() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_capacity;
}

void set_capacity(size_t n) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_capacity = n < 16 ? 16 : n;
    while (g_lines.size() > g_capacity) g_lines.pop_front();
}

ReadResult read(uint64_t cursor, size_t limit, const std::string &re, std::string &err) {
    ReadResult r{{}, cursor, 0, 0};
    std::unique_ptr<std::regex> rx;
    if (!re.empty()) {
        try {
            rx = std::make_unique<std::regex>(re, std::regex::ECMAScript | std::regex::icase);
        } catch (const std::exception &e) {
            err = std::string("invalid regex: ") + e.what();
            return r;
        }
    }
    // Copy the candidate lines under the lock, then run the regex outside it: the lock is taken by every engine print, and
    // a slow pattern over a large buffer must not stall the game. Without a regex only `limit` lines are needed.
    std::vector<Line> cand;
    uint64_t snapshot_next;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        uint64_t oldest = g_lines.empty() ? g_next_seq : g_lines.front().seq;
        r.oldest = oldest;
        snapshot_next = g_next_seq;
        if (cursor > g_next_seq) cursor = g_next_seq;
        if (cursor < oldest) {
            r.lost = oldest - cursor;
            cursor = oldest;
        }
        size_t first = (size_t)(cursor - oldest);
        size_t n = g_lines.size() - first;
        if (!rx && n > limit) n = limit;
        cand.reserve(n);
        for (size_t i = first; i < first + n; ++i) cand.push_back(g_lines[i]);
    }
    uint64_t c = cursor;
    try {
        for (const Line &l : cand) {
            c = l.seq + 1;
            if (rx && !std::regex_search(l.text, *rx)) continue;
            r.lines.push_back(l);
            if (r.lines.size() >= limit) break;
        }
    } catch (const std::regex_error &e) {  // e.g. error_complexity / error_stack on a pathological pattern
        err = std::string("regex too expensive to run: ") + e.what();
        r.lines.clear();
        r.next_cursor = cursor;
        return r;
    }
    r.next_cursor = r.lines.size() >= limit ? c : snapshot_next;
    return r;
}

void set_map_load_source(uint64_t (*fn)()) { g_map_load_fn = fn; }

void push_error(const json &e) {
    std::lock_guard<std::mutex> lk(g_emu);
    json x = e;
    if (!x.contains("map_load")) x["map_load"] = g_map_load_fn ? g_map_load_fn() : 0;
    x["seq"] = g_err_next++;
    if (!x.contains("time")) x["time"] = uptime();
    g_errors.push_back(std::move(x));
    while (g_errors.size() > kErrCapacity) g_errors.pop_front();
}

json errors_since(uint64_t cursor, size_t limit) {
    std::lock_guard<std::mutex> lk(g_emu);
    json out = json::array();
    uint64_t oldest = g_errors.empty() ? g_err_next : g_errors.front()["seq"].get<uint64_t>();
    uint64_t from = cursor < g_err_floor ? g_err_floor : cursor;
    uint64_t lost = from < oldest ? oldest - from : 0;
    uint64_t next = cursor < oldest ? oldest : cursor;
    for (const auto &e : g_errors) {
        uint64_t s = e["seq"].get<uint64_t>();
        if (s < cursor) continue;
        if (out.size() >= limit) break;
        out.push_back(e);
        next = s + 1;
    }
    if (out.size() < limit) next = g_err_next;
    return json{{"errors", out}, {"next_cursor", next}, {"lost", lost}};
}

uint64_t errors_cursor() {
    std::lock_guard<std::mutex> lk(g_emu);
    return g_err_next;
}

void errors_clear() {
    std::lock_guard<std::mutex> lk(g_emu);
    g_errors.clear();
    g_err_floor = g_err_next;
}

void set_parse_errors_from_console(bool on) { g_parse_errors = on; }

}  // namespace console

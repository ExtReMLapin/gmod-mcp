// FP7137 workaround: see fp7137.h and docs/FP7137.md. Delete this file with its header to remove the workaround.
#include "fp7137.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include "platform/platform.h"

using json = nlohmann::json;

namespace fp7137 {

namespace {

std::mutex g_mu;
std::string g_dir;  // FP7137: folder of the shared state
std::atomic<uint64_t> g_seq{0};

int64_t wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string state_path() { return g_dir + "/fp7137.state"; }

std::string dir_copy() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_dir;
}

// Lock shared by every bridge of this user around a read-modify-write of the state file. The OS releases it when a
// process dies. If it cannot be taken (read-only folder...) the caller carries on without the workaround.
class FileLock {
#ifdef _WIN32
    HANDLE h_ = INVALID_HANDLE_VALUE;

  public:
    explicit FileLock(const std::string &path) {
        h_ = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
        OVERLAPPED ov{};
        if (h_ != INVALID_HANDLE_VALUE && !LockFileEx(h_, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &ov)) {
            CloseHandle(h_);
            h_ = INVALID_HANDLE_VALUE;
        }
    }
    ~FileLock() {
        if (h_ == INVALID_HANDLE_VALUE) return;
        OVERLAPPED ov{};
        UnlockFileEx(h_, 0, 1, 0, &ov);
        CloseHandle(h_);
    }
    bool ok() const { return h_ != INVALID_HANDLE_VALUE; }
#else
    int fd_ = -1;

  public:
    explicit FileLock(const std::string &path) {
        fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd_ >= 0 && ::flock(fd_, LOCK_EX) != 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    ~FileLock() {
        if (fd_ < 0) return;
        ::flock(fd_, LOCK_UN);
        ::close(fd_);
    }
    bool ok() const { return fd_ >= 0; }
#endif
};

bool owner_alive(const Entry &e) {
    uint64_t now = plat::process_start_stamp(e.pid);
    if (now == 0) return false;                         // no such process
    if (e.stamp <= 1 || now <= 1) return true;          // a start stamp is unknown: the pid is all there is
    return now == e.stamp;                              // another start time: the pid was reused
}

State load_state() {
    std::string s;
    if (!plat::read_file(state_path(), s)) return State();
    return parse_state(s);
}
void store_state(const State &st) {
    std::string tmp = state_path() + "." + std::to_string(plat::process_id()) + ".tmp";
    if (!plat::write_file(tmp, serialize_state(st))) return;
    std::error_code ec;
    std::filesystem::rename(std::filesystem::u8path(tmp), std::filesystem::u8path(state_path()), ec);
    if (ec) plat::remove_file(tmp);
}

// Read, prune, let fn change the state, write back (if anything changed). All under the shared file lock.
template <class Fn>
bool with_state(Fn fn) {
    std::string dir = dir_copy();
    if (dir.empty()) return false;
    plat::make_dirs(dir);
    FileLock lock(dir + "/fp7137.lock");
    if (!lock.ok()) return false;
    State st = load_state();
    bool changed = prune(st, wall_ms(), owner_alive);
    if (fn(st)) changed = true;
    if (changed) store_state(st);
    return true;
}

void release_entry(const std::string &tag) {
    uint32_t me = plat::process_id();
    with_state([&](State &st) {
        auto it = std::remove_if(st.entries.begin(), st.entries.end(), [&](const Entry &e) { return e.pid == me && e.tag == tag; });
        bool any = it != st.entries.end();
        st.entries.erase(it, st.entries.end());
        return any;
    });
}

std::string clean_tag(std::string t) {
    for (auto &c : t)
        if (c <= ' ' || c == 0x7f) c = '_';
    return t.empty() ? "-" : t;
}

}  // namespace

bool enabled() {
    const char *v = std::getenv("GMOD_MCP_FP7137_WORKAROUND");
    return !(v && std::string(v) == "0");
}

int64_t settle_ms() {
    const char *v = std::getenv("GMOD_MCP_FP7137_SETTLE_MS");
    if (v && *v) {
        long n = std::atol(v);
        if (n >= 0) return n;
    }
    return kDefaultSettleMs;
}

void set_dir(const std::string &dir) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_dir = dir;
}

// --- Pure logic ---

State parse_state(const std::string &text) {
    State s;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream ls(line);
        std::string tag;
        ls >> tag;
        if (tag == "S") {
            long long v = 0;
            if (ls >> v && v >= 0) s.last_start_ms = v;
        } else if (tag == "E") {
            std::string kind;
            unsigned long long pid = 0, stamp = 0;
            long long until = 0;
            std::string t;
            if (ls >> kind >> pid >> stamp >> until >> t && kind.size() == 1 && (kind[0] == 'L' || kind[0] == 'M') && pid > 0 &&
                pid <= 0xFFFFFFFFull)
                s.entries.push_back(Entry{kind[0], (uint32_t)pid, (uint64_t)stamp, (int64_t)until, t});
        }
    }
    return s;
}

std::string serialize_state(const State &s) {
    std::ostringstream o;
    o << "S " << s.last_start_ms << "\n";
    for (const auto &e : s.entries) o << "E " << e.kind << " " << e.pid << " " << e.stamp << " " << e.until_ms << " " << clean_tag(e.tag) << "\n";
    return o.str();
}

bool prune(State &s, int64_t now_ms, const std::function<bool(const Entry &)> &alive) {
    auto it = std::remove_if(s.entries.begin(), s.entries.end(), [&](const Entry &e) { return e.until_ms <= now_ms || !alive(e); });
    bool any = it != s.entries.end();
    s.entries.erase(it, s.entries.end());
    return any;
}

Verdict launch_verdict(const State &s, int64_t now_ms) {
    (void)now_ms;
    Verdict v;
    std::string what;
    int n = 0;
    for (const auto &e : s.entries)
        if (e.kind == 'M') {
            what += (n++ ? ", " : "") + e.tag;
        }
    if (n == 0) return v;
    v.wait = true;
    v.code = "map_load_in_progress";
    v.reason = "a map is loading in another game (" + what +
               "): starting a game now would make it release its graphics resources and could crash it (Garry's Mod "
               "issue 7137)";
    return v;
}

Verdict map_load_verdict(const State &s, int64_t now_ms, int64_t settle) {
    Verdict v;
    for (const auto &e : s.entries)
        if (e.kind == 'L') {
            v.wait = true;
            v.code = "launch_in_progress";
            v.reason = "a game is being started (instances_launch in progress): starting a game makes the other games release "
                       "their graphics resources for several seconds, and a map load ending meanwhile can crash (Garry's Mod "
                       "issue 7137)";
            v.ready_at_ms = 0;
            return v;
        }
    int64_t ready = s.last_start_ms > 0 ? s.last_start_ms + settle : 0;
    if (ready > now_ms) {
        v.wait = true;
        v.code = "launch_in_progress";
        v.reason = "a game process was started " + std::to_string((now_ms - s.last_start_ms) / 1000) +
                   " s ago: the other games release and restore their graphics resources for several seconds after a start, "
                   "and a map load ending meanwhile can crash (Garry's Mod issue 7137); waiting " + std::to_string(settle / 1000) +
                   " s after the last start";
        v.ready_at_ms = ready;
    }
    return v;
}

bool exceeds_deadline(const Verdict &v, int64_t deadline_ms) { return v.wait && v.ready_at_ms > 0 && v.ready_at_ms > deadline_ms; }

size_t parallel_limit(size_t n, bool workaround_enabled) {
    if (!workaround_enabled) return n;
    return std::min(n, kMaxParallelMaps);
}

void for_each_limited(size_t n, const std::function<void(size_t)> &fn) {
    size_t limit = parallel_limit(n, enabled());
    std::atomic<size_t> next{0};
    std::vector<std::thread> th;
    for (size_t w = 0; w < limit; ++w)
        th.emplace_back([&] {
            for (size_t i; (i = next.fetch_add(1)) < n;) fn(i);
        });
    for (auto &t : th) t.join();
}

// --- Gate ---

void Gate::enter(Kind kind, const std::string &tag, double budget_s, double hold_s, const std::atomic<bool> *cancelled,
                 const Notify &notify) {
    if (!enabled()) return;
    auto t0 = std::chrono::steady_clock::now();
    auto waited = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    int64_t deadline = wall_ms() + (int64_t)(budget_s * 1000);
    int64_t settle = settle_ms();
    std::string my_tag = clean_tag(tag) + "#" + std::to_string(g_seq.fetch_add(1));
    double next_note = 0;
    bool noted = false;
    for (;;) {
        Verdict v;
        bool registered = false;
        bool locked = with_state([&](State &st) {
            int64_t now = wall_ms();
            v = kind == Kind::Launch ? launch_verdict(st, now) : map_load_verdict(st, now, settle);
            if (v.wait) return false;
            st.entries.push_back(Entry{kind == Kind::Launch ? 'L' : 'M', plat::process_id(), plat::process_start_stamp(plat::process_id()),
                                       now + (int64_t)(hold_s * 1000), my_tag});
            registered = true;
            return true;
        });
        if (!locked || registered) {  // free to go (or no shared state available: carry on without the workaround)
            waited_s_ = noted ? waited() : 0;
            if (registered) {
                entered_ = true;
                tag_ = my_tag;
                until_ms_ = wall_ms() + (int64_t)(hold_s * 1000);
            }
            return;
        }
        reason_ = v.reason;
        double w = waited();
        if (exceeds_deadline(v, deadline) || wall_ms() >= deadline) {
            ok_ = false;
            code_ = v.code;
            waited_s_ = w;
            return;
        }
        if (cancelled && cancelled->load()) {
            ok_ = false;
            aborted_ = true;
            waited_s_ = w;
            return;
        }
        if (!noted || w >= next_note) {
            if (notify) notify(w, "waiting: " + v.reason);
            noted = true;
            next_note = w + 2.0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void Gate::note_started() {
    if (!enabled()) return;
    with_state([&](State &st) {
        st.last_start_ms = wall_ms();
        return true;
    });
}

void Gate::release() {
    if (!entered_) return;
    entered_ = false;
    release_entry(tag_);
}

void Gate::release_when_done(std::function<bool()> running) {
    if (!entered_) return;
    entered_ = false;
    std::string tag = tag_;
    int64_t until = until_ms_;
    std::thread([tag, until, running = std::move(running)] {
        while (wall_ms() < until && running()) std::this_thread::sleep_for(std::chrono::seconds(1));
        release_entry(tag);
    }).detach();
}

void Gate::annotate(json &res) const {
    if (waited_s_ < 0.05 || !res.is_object() || !res.contains("structuredContent") || !res["structuredContent"].is_object()) return;
    json &sc = res["structuredContent"];
    sc["waited_s"] = std::round(waited_s_ * 10) / 10;
    sc["waited_reason"] = reason_;
    // The text content is the JSON of the structured one in the bridge's and the game's answers: keep them equal.
    if (res.contains("content") && res["content"].is_array() && !res["content"].empty() && res["content"][0].is_object())
        res["content"][0]["text"] = sc.dump(-1, ' ', false, json::error_handler_t::replace);
}

json Gate::error_result() const {
    json sc{{"error", {{"code", code_},
                       {"message", reason_ + ". Nothing was started: waited " + std::to_string((int)waited_s_) +
                                       " s and the call's timeout would be exceeded; retry later (or set "
                                       "GMOD_MCP_FP7137_WORKAROUND=0 to skip this wait)."}}},
            {"waited_s", std::round(waited_s_ * 10) / 10},
            {"waited_reason", reason_}};
    return json{{"content", json::array({json{{"type", "text"}, {"text", sc.dump(-1, ' ', false, json::error_handler_t::replace)}}})},
                {"structuredContent", sc},
                {"isError", true}};
}

}  // namespace fp7137

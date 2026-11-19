// Multi-instance coordination.
//
// The first instance that binds the base port is the coordinator. Every other instance (member) sends a heartbeat to
// the base port every 2 s (registration is an upsert, authenticated with the shared bearer token). When the
// coordinator stops answering, members try to bind the base port: the exclusive bind guarantees a single winner,
// and the others simply re-register with it on their next heartbeat.
//
// Hand-over (issue #6): a member tries to bind the base port at its FIRST failed heartbeat (the bind only succeeds when
// nobody holds the port, so it never takes a slow coordinator over; it used to wait for 3 failures, 9 s on Windows where
// a closed loopback port takes 2 s to refuse). The new coordinator then asks every other port of the range to
// re-register at once, and for a few seconds it answers `coordinator_changing` (retryable) rather than
// `unknown_instance` for an instance that did not register yet.
#include "coordinator.h"

#ifdef _WIN32
#include <winsock2.h>
#endif
#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#include "agent.h"
#include "console.h"
#include "crashinfo.h"
#include "gmod_mcp_reply.h"
#include "health.h"
#include "lowmem.h"
#include "http_server.h"
#include "jobs.h"
#include "log.h"
#include "mcp.h"
#include "platform/platform.h"
#include "realms.h"

using nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace coordinator {

static const double kHeartbeatS = 2.0;
static const double kDeadAfterS = 10.0;
static const double kFastRetryS = 0.3;          // heartbeat period after a failed one (the coordinator may be gone)
static const double kHandoverGraceS = 8.0;      // after a take-over: an unknown instance may just not have re-registered yet
static const double kHandoverRetryS = 1.0;      // retry_after_s advised during the grace period

// Issue #8: waits that tests shorten (set_timing_for_tests).
static std::atomic<double> g_dead_after_s{kDeadAfterS};
static std::atomic<double> g_hung_after_s{0};       // 0 = health::kHungAfterS / kHungAfterLoadingS
static const double kInflightSilentS = 30.0;         // a call in flight is cut when the instance sent no heartbeat for this long
static std::atomic<double> g_inflight_silent_s{kInflightSilentS};
static std::atomic<double> g_low_fps_shown_s{health::kLowFpsShownS};

static double steady_s() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

static double unix_now() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

// How an instance ended, worked out once when its process is found dead (crashinfo::judge).
struct Ending {
    crashinfo::Verdict verdict;
    bool exit_known = false;
    uint32_t exit_code = 0;
    double ended_unix = 0;
};

// What the coordinator knows of one instance's process. Shared between its Member entry and, after it is removed, its
// Gone entry; the watch handle (kept open while the process lives) lets its exit code be read after it ended.
struct Tracked {
    std::string id, mode, game_dir;
    uint32_t pid = 0;
    uint64_t stamp = 0;  // plat::process_start_stamp(pid) taken while it answered: tells the process from a later pid reuse
    void *watch = nullptr;
    double start_unix = 0;
    double last_seen_unix = 0;
    bool requested = false;  // game_quit / game_terminate was relayed to it
    bool goodbye = false;    // its module said it is closing
    bool ended = false;
    Ending ending;
    ~Tracked() { plat::process_watch_close(watch); }
};

struct Member {
    json info;  // id, mode, port, pid, map, realms
    Clock::time_point last_seen;
    std::shared_ptr<Tracked> t;
    health::FpsWatch fps_watch;  // issue #8: how long its frame rate stayed low
};

static std::mutex g_mu;
static std::map<std::string, Member> g_members;
static health::FpsWatch g_self_fps;  // this instance's own frame rate, fed by the loop (needs g_mu)
static std::set<std::string> g_claimed_dumps;  // dumps already given to a dead instance (two crashes do not share one)
static std::atomic<bool> g_run{false};
static std::thread g_thread;
static std::atomic<bool> g_wake{false};  // FP7137 (issue #7): heartbeat now

void wake() { g_wake = true; }  // FP7137
static std::atomic<int64_t> g_takeover_ms{0};  // steady-clock ms of the last take-over of the base port (0 = never)
static std::mutex g_cid_mu;
static std::string g_coordinator_id;  // member side: the coordinator named by the last heartbeat answer

static int64_t steady_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count() + 1; }

bool is_coordinator() {
    int base = agent::config().base_port;
    return httpsrv::port() == base || (httpsrv::has_extra() && httpsrv::extra_port() == base);
}

bool in_handover() {
    int64_t t = g_takeover_ms.load();
    return t != 0 && (double)(steady_ms() - t) / 1000.0 < kHandoverGraceS;
}

void note_takeover_for_tests(bool on) { g_takeover_ms = on ? steady_ms() : 0; }

void set_timing_for_tests(double dead_after_s, double hung_after_s, double inflight_silent_s, double low_fps_shown_s) {
    g_dead_after_s = dead_after_s > 0 ? dead_after_s : kDeadAfterS;
    g_hung_after_s = hung_after_s > 0 ? hung_after_s : 0.0;
    g_inflight_silent_s = inflight_silent_s > 0 ? inflight_silent_s : kInflightSilentS;
    g_low_fps_shown_s = low_fps_shown_s > 0 ? low_fps_shown_s : health::kLowFpsShownS;
}

json handover_info() {
    if (!in_handover()) return json();
    return json{{"state", "changing"}, {"retry_after_s", kHandoverRetryS},
                {"note", "this instance just took over the coordinator role: the other instances are re-registering, the list is "
                         "incomplete for a few seconds (calls with `instance` answer coordinator_changing meanwhile)"}};
}

std::string coordinator_id() {
    if (is_coordinator()) return agent::config().instance_id;
    std::lock_guard<std::mutex> lk(g_cid_mu);
    return g_coordinator_id;
}

static json self_info() {
    const auto &c = agent::config();
    json realms_j = json::object();
    for (int r = 0; r < realms::COUNT; ++r) realms_j[realms::name(r)] = agent::realm_present(r);
    // Health at a glance (sent with every heartbeat): agents pick a usable instance without one status call each.
    json fps = jobs::frame_stats()["fps"];
    double since = jobs::seconds_since_last_frame();
    return json{{"id", c.instance_id}, {"mode", agent::mode_name()}, {"port", c.port}, {"pid", plat::process_id()},
                {"map", agent::current_map()}, {"realms", realms_j}, {"agent_version", c.version},
                {"uptime_s", (int64_t)console::uptime()}, {"fps", fps.is_number() ? json((int)(fps.get<double>() + 0.5)) : json(nullptr)},
                {"main_thread_stalled", since > 5.0}, {"last_tick_age_s", since < 0 ? 0.0 : std::round(since * 100) / 100},  // issue #8
                {"lua_errors", console::errors_cursor()},
                {"loading", agent::map_loading()},  // FP7137 (issue #7): a map load is in progress, whatever started it
                {"game_dir", c.game_dir}};  // read by the coordinator (where the crash dumps are), not shown by instances_list
}

// Members recently removed (process gone, or no heartbeat): so that a later call gets a useful answer, and a crash stays
// visible in instances_list for a while.
struct Gone {
    std::shared_ptr<Tracked> t;
    Clock::time_point when;  // last heartbeat
    json info;               // what its last heartbeat said (issue #8: an instance that went silent but whose process lives is still listed)
};
static std::map<std::string, Gone> g_gone;
static const double kCrashShownS = 3600.0;

// Is the process gone? When it is, works out once how it ended. Needs g_mu.
static bool finalize_locked(Tracked &t) {
    if (t.ended) return true;
    plat::ProcInfo pi;
    bool have = t.watch && plat::process_watch_query(t.watch, pi, 0);
    bool dead = have ? pi.exited : (t.pid != 0 && !same_process_running(plat::process_start_stamp(t.pid), t.stamp));
    if (!dead) return false;
    crashinfo::Facts f;
    f.requested = t.requested;
    f.goodbye = t.goodbye;
    f.exit_known = have && pi.exited;
    f.exit_code = f.exit_known ? pi.exit_code : 0;
    f.exit_unix = f.exit_known ? pi.exit_unix : 0;
    f.start_unix = have && pi.start_unix > 0 ? pi.start_unix : t.start_unix;
    f.last_seen_unix = t.last_seen_unix;
    f.now_unix = unix_now();
    t.ending.exit_known = f.exit_known;
    t.ending.exit_code = f.exit_code;
    t.ending.ended_unix = f.exit_known && f.exit_unix > 0 ? f.exit_unix : f.last_seen_unix;
    // The dumps are only read when nobody asked for the end (a quit never looks at the disk).
    std::vector<crashinfo::Dump> dumps;
    if (!f.requested && !f.goodbye) dumps = crashinfo::list_dumps(crashinfo::dump_dirs(t.game_dir));
    t.ending.verdict = crashinfo::judge(f, dumps, g_claimed_dumps);
    if (!t.ending.verdict.dump.empty()) {
        if (g_claimed_dumps.size() > 500) g_claimed_dumps.clear();
        g_claimed_dumps.insert(t.ending.verdict.dump);
    }
    t.ended = true;
    logx::info("instance %s (pid %u) ended: %s%s", t.id.c_str(), t.pid,
               t.ending.verdict.cause == crashinfo::Cause::Crashed ? "crashed" : t.ending.verdict.cause == crashinfo::Cause::Quit ? "quit" : "unknown cause",
               t.ending.verdict.dump.empty() ? "" : (", dump " + t.ending.verdict.dump).c_str());
    return true;
}

static void purge_locked() {
    auto now = Clock::now();
    for (auto it = g_members.begin(); it != g_members.end();) {
        double age = std::chrono::duration<double>(now - it->second.last_seen).count();
        bool dead = finalize_locked(*it->second.t);  // the process ended: no need to wait for the heartbeat timeout
        if (age > g_dead_after_s || dead) {
            logx::info("instance %s removed (%s)", it->first.c_str(), dead ? "its process ended" : ("no heartbeat for " + std::to_string((int)age) + " s").c_str());
            g_gone[it->first] = Gone{it->second.t, it->second.last_seen, it->second.info};
            it = g_members.erase(it);
        } else {
            ++it;
        }
    }
    // Forget old ones: after an hour, and past 200 entries the oldest.
    for (auto it = g_gone.begin(); it != g_gone.end();)
        it = std::chrono::duration<double>(now - it->second.when).count() > kCrashShownS ? g_gone.erase(it) : std::next(it);
    while (g_gone.size() > 200) {
        auto oldest = g_gone.begin();
        for (auto it = g_gone.begin(); it != g_gone.end(); ++it)
            if (it->second.when < oldest->second.when) oldest = it;
        g_gone.erase(oldest);
    }
}

static double round1(double v) { return std::round(v * 10) / 10; }

static std::string hung_note(uint32_t pid, double stalled_s, double limit_s, bool silent) {
    std::string pids = pid ? "{pids: [" + std::to_string(pid) + "]}" : "{pids}";
    return std::string(silent ? "no heartbeat for " : "its main thread has been blocked for ") + std::to_string((int)stalled_s) + " s (hung after " +
           std::to_string((int)limit_s) + " s" + (silent ? "; the process still runs: frozen as a whole, crash dialog or debugger" :
                                                          ": a map load that never ends, an infinite loop or a crash dialog") +
           "). It will not recover by itself: close it with instances_close " + pids + " (or game_terminate {instance}) and relaunch a client with "
           "instances_launch; anything measured on it since it stalled is not valid.";
}

static std::string unresponsive_note(double silent_s, double limit_s) {
    return "no heartbeat for " + std::to_string((int)silent_s) + " s but its process still runs: busy or frozen. A call to it may fail meanwhile; it is "
           "listed as hung after " + std::to_string((int)limit_s) + " s without a sign of life (it comes back by itself if it was only a long load).";
}

// Issue #8: stalled_s, fps_low_s and, past the limit, state "hung" with the advice. `silent`: its heartbeats stopped.
static void add_health(json &e, double last_tick_age_s, double hb_age_s, bool loading, bool silent, double fps_low_s, uint32_t pid) {
    health::Assessment a = health::assess(last_tick_age_s, hb_age_s, loading, g_hung_after_s, silent);
    e["stalled_s"] = round1(a.stalled_s);
    e["fps_low_s"] = round1(fps_low_s);
    if (a.hung) {
        e["state"] = "hung";
        e["note"] = hung_note(pid, a.stalled_s, a.hung_after_s, silent && last_tick_age_s < health::kStalledShownS);
    }
}

json instances() {
    json arr = json::array();
    json me = self_info();
    me.erase("game_dir");
    me["self"] = true;
    me["state"] = "running";
    me["role"] = is_coordinator() ? "coordinator" : "member";
    me["heartbeat_age_s"] = 0;
    std::lock_guard<std::mutex> lk(g_mu);
    add_health(me, me.value("last_tick_age_s", 0.0), 0, me.value("loading", false), false, g_self_fps.low_s(steady_s(), g_low_fps_shown_s), me.value("pid", 0u));
    arr.push_back(me);
    purge_locked();
    auto now = Clock::now();
    for (auto &kv : g_members) {
        json e = kv.second.info;
        e["self"] = false;
        e["state"] = "running";
        e["role"] = "member";
        double hb_age = std::chrono::duration<double>(now - kv.second.last_seen).count();
        e["heartbeat_age_s"] = hb_age;
        add_health(e, e.value("last_tick_age_s", e.value("main_thread_stalled", false) ? 5.0 : 0.0), hb_age, e.value("loading", false), false,
                   kv.second.fps_watch.low_s(steady_s(), g_low_fps_shown_s), e.value("pid", 0u));
        arr.push_back(e);
    }
    for (auto &kv : g_gone) {
        Tracked &t = *kv.second.t;
        if (g_members.count(kv.first)) continue;
        if (!finalize_locked(t)) {
            // Issue #8: no heartbeat any more but the process lives (frozen as a whole, crash dialog): it used to vanish from the
            // list. A game that is closing (goodbye, or asked to end) is not shown.
            if (t.goodbye || t.requested) continue;
            json e = kv.second.info;
            double silent_s = std::chrono::duration<double>(now - kv.second.when).count();
            e["self"] = false;
            e["role"] = "member";
            e["state"] = "unresponsive";
            e["heartbeat_age_s"] = silent_s;
            add_health(e, e.value("last_tick_age_s", 0.0), silent_s, e.value("loading", false), true, 0, t.pid);
            if (e["state"] == "unresponsive") e["note"] = unresponsive_note(silent_s, health::assess(0, 0, e.value("loading", false), g_hung_after_s).hung_after_s);
            arr.push_back(e);
            continue;
        }
        if (t.ending.verdict.cause != crashinfo::Cause::Crashed) continue;
        // Instances that crashed (a clean quit just disappears, as before).
        json ce = crashinfo::crashed_entry(t.id, t.mode, t.pid, std::max(0.0, unix_now() - t.ending.ended_unix), t.ending.verdict, t.ending.exit_known,
                                    t.ending.exit_code);
        json mem = lowmem::memory_json(lowmem::current());  // issue #8
        if (!mem.is_null()) ce["crash"]["memory"] = mem;
        arr.push_back(ce);
    }
    return arr;
}

json leaving_note() {
    if (!is_coordinator()) return json();
    json ids = json::array();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        purge_locked();
        for (auto &kv : g_members) ids.push_back(kv.first);
    }
    if (ids.empty()) return json();
    return json{{"coordinator_moved", true}, {"remaining_instances", ids},
                {"warning", "this instance is the coordinator (it holds the base port and the directory of instances): when it ends, "
                            "the other instances elect a new one within about a second; calls with `instance` answer "
                            "coordinator_changing (retryable) until they re-registered"}};
}

static std::unique_ptr<httplib::Client> client(int port, double read_timeout_s) {
    auto cli = std::make_unique<httplib::Client>("127.0.0.1", port);
    cli->set_connection_timeout(1, 0);
    cli->set_read_timeout((time_t)read_timeout_s, 0);
    cli->set_write_timeout(10, 0);
    cli->set_bearer_token_auth(agent::config().token);
    return cli;
}

// Issue #8: one heartbeat's frame rate into the member's low-fps watch (not while it loads or its main thread is blocked: those
// have their own flags).
static void feed_fps(Member &m, const json &info) {
    double fps = info.contains("fps") && info["fps"].is_number() ? info["fps"].get<double>() : -1.0;
    double tick = info.value("last_tick_age_s", info.value("main_thread_stalled", false) ? 5.0 : 0.0);
    m.fps_watch.feed(steady_s(), fps, info.value("loading", false) || tick >= health::kStalledShownS);
}

// Coordinator side: heartbeat / registration (upsert).
static json on_heartbeat(const json &in) {
    if (!is_coordinator()) return json{{"ok", false}, {"error", "not_coordinator"}};
    if (!in.contains("id") || !in.contains("port")) return json{{"ok", false}, {"error", "bad_request"}};
    std::string id = in["id"];
    if (id == agent::config().instance_id) return json{{"ok", true}};
    std::lock_guard<std::mutex> lk(g_mu);
    auto old = g_members.find(id);
    if (in.value("closing", false)) {  // the instance's module closes with the game: a clean shutdown, not a crash
        if (old != g_members.end()) old->second.t->goodbye = true;
        auto gone = g_gone.find(id);  // issue #8: it may have been listed as unresponsive meanwhile
        if (gone != g_gone.end()) gone->second.t->goodbye = true;
        return json{{"ok", true}, {"coordinator", agent::config().instance_id}};
    }
    json info = in;
    info.erase("game_dir");
    info.erase("closing");
    if (old == g_members.end()) {
        auto t = std::make_shared<Tracked>();
        t->id = id;
        t->mode = in.value("mode", "client");
        t->game_dir = in.value("game_dir", "");
        t->pid = in.value("pid", 0u);
        // The stamp and the handle are taken at registration, while the process surely runs: after its death neither can be read.
        t->stamp = plat::process_start_stamp(t->pid);
        t->watch = plat::process_watch_open(t->pid);
        plat::ProcInfo pi;
        if (t->watch && plat::process_watch_query(t->watch, pi, 0) && pi.start_unix > 0) t->start_unix = pi.start_unix;
        t->last_seen_unix = unix_now();
        g_members[id] = Member{info, Clock::now(), t, health::FpsWatch()};
        feed_fps(g_members[id], info);
        g_gone.erase(id);
        logx::info("instance %s registered (port %d, %s)", id.c_str(), in.value("port", 0), in.value("mode", "?").c_str());
    } else {
        Tracked &t = *old->second.t;
        if (t.stamp == 0) t.stamp = plat::process_start_stamp(t.pid);
        t.last_seen_unix = unix_now();
        old->second.info = info;
        old->second.last_seen = Clock::now();
        feed_fps(old->second, info);
    }
    return json{{"ok", true}, {"coordinator", agent::config().instance_id}};
}

bool same_process_running(uint64_t now, uint64_t recorded) {
    if (now == 0) return false;
    if (now == 1 || recorded == 0 || recorded == 1) return true;  // unreadable or never recorded: cannot tell
    return now == recorded;                                       // another stamp: the pid was reused
}

std::string gone_message(const std::string &id, uint32_t pid, double seconds_ago, bool process_alive) {
    std::string who = "instance " + id + (pid ? " (pid " + std::to_string(pid) + ")" : "");
    std::string when = " stopped answering " + std::to_string((int)seconds_ago) + " s ago";
    if (!process_alive)
        return who + when + ": its process has exited (closed, or crashed: see garrysmod/console.log or the crash dialog). "
                            "Relaunch a client with instances_launch.";
    return who + when + " but its process is still running: frozen (infinite loop, long load) or busy. Check it with "
                        "status {instance}; game_terminate {instance} ends it.";
}

static json wrap_err(const std::string &code, const std::string &msg, const json &details = json()) {
    json e{{"error", {{"code", code}, {"message", msg}}}};
    if (!details.is_null()) e["error"]["details"] = details;
    return json{{"content", json::array({json{{"type", "text"}, {"text", e.dump()}}})}, {"structuredContent", e}, {"isError", true}};
}

// The answer for a call to an instance whose process is gone (needs g_mu): crashed, quit, or ended for a reason unknown.
static json ended_error(Tracked &t) {
    double ago = std::max(0.0, unix_now() - t.ending.ended_unix);
    const crashinfo::Verdict &v = t.ending.verdict;
    if (v.cause == crashinfo::Cause::Crashed) {
        json details{{"instance", t.id}, {"pid", t.pid}, {"dump", v.dump.empty() ? json(nullptr) : json(v.dump)}, {"evidence", v.evidence}};
        if (t.ending.exit_known) details["exit_code"] = t.ending.exit_code;
        std::string message = crashinfo::crash_message(t.mode, t.id, t.pid, ago, v, t.ending.exit_known, t.ending.exit_code);
        lowmem::annotate_crash(lowmem::current(), details, message);  // issue #8: free memory now, and a hint when it is low
        return wrap_err(crashinfo::crash_code(t.mode), message, details);
    }
    if (v.cause == crashinfo::Cause::Quit)
        return wrap_err("instance_gone", crashinfo::quit_message(t.id, t.pid, ago, v.evidence.empty() ? "clean exit" : v.evidence[0]));
    return wrap_err("instance_gone", gone_message(t.id, t.pid, ago, false));
}

// Relay of a tools/call to another instance (coordinator only).
static json relay(const std::string &instance, const std::string &tool, const json &args) {
    if (!is_coordinator()) return wrap_err("not_coordinator", "this instance is not the coordinator: call the coordinator (base port)");
    int port = 0;
    std::shared_ptr<Tracked> t;
    Clock::time_point last_seen = Clock::now();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        purge_locked();
        auto it = g_members.find(instance);
        if (it == g_members.end()) {
            auto g = g_gone.find(instance);
            if (g != g_gone.end()) {
                Tracked &gt = *g->second.t;
                if (finalize_locked(gt)) return ended_error(gt);
                double ago = std::chrono::duration<double>(Clock::now() - g->second.when).count();
                return wrap_err("instance_gone", gone_message(instance, gt.pid, ago, true));
            }
            if (in_handover())
                return wrap_err("coordinator_changing",
                                "the coordinator role just moved to this instance (" + agent::config().instance_id + ") and " + instance +
                                    " has not registered with it yet: retry in " + std::to_string((int)kHandoverRetryS) +
                                    " s (nothing was sent to it)",
                                json{{"retry_after_s", kHandoverRetryS}, {"instance", instance}});
            return wrap_err("unknown_instance", "unknown instance: " + instance + " (see instances_list)");
        }
        port = it->second.info.value("port", 0);
        t = it->second.t;
        last_seen = it->second.last_seen;
        // Asked to end: when its process goes, that is not a crash.
        if (tool == "game_quit" || tool == "game_terminate") t->requested = true;
    }
    auto cli = client(port, kRelayReadTimeoutS);
    json req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", {{"name", tool}, {"arguments", args}}}};
    // Issue #8: the call waits for the instance's answer (up to kRelayReadTimeoutS), but watches the instance meanwhile: when its process ends
    // (the connection may stay open: a crash dialog holds the process, a handle may be inherited) or its heartbeats stop for
    // good, the call is cut and answered at once (client_crashed / instance_hung) instead of at its own timeout.
    std::string body_text = req.dump();
    auto fut = std::async(std::launch::async, [&] { return cli->Post("/mcp", body_text, "application/json"); });
    json cut;  // the answer when the call was cut
    while (fut.wait_for(std::chrono::milliseconds(250)) != std::future_status::ready) {
        std::lock_guard<std::mutex> lk(g_mu);
        if (finalize_locked(*t)) {
            cut = ended_error(*t);
        } else {
            double silent = -1;
            auto m = g_members.find(instance);
            if (m == g_members.end()) {
                auto g = g_gone.find(instance);
                if (g != g_gone.end() && g->second.t == t) silent = std::chrono::duration<double>(Clock::now() - g->second.when).count();
            } else if (m->second.t == t) {
                silent = std::chrono::duration<double>(Clock::now() - m->second.last_seen).count();
            }
            if (silent >= g_inflight_silent_s && !t->goodbye)
                cut = wrap_err("instance_hung",
                               "instance " + instance + (t->pid ? " (pid " + std::to_string(t->pid) + ")" : "") + " sent no heartbeat for " + std::to_string((int)silent) +
                                   " s while this call was in flight, and its process still runs: it is frozen as a whole (crash dialog, debugger, hung driver) and will not "
                                   "answer, so the call was cut. Close it with game_terminate {instance} or instances_close {pids:[" + std::to_string(t->pid) +
                                   "]} and relaunch a client with instances_launch (instances_list shows whether it came back meanwhile).",
                               json{{"instance", instance}, {"pid", t->pid}, {"silent_s", round1(silent)}});
        }
        if (!cut.is_null()) break;
    }
    if (!cut.is_null()) {
        do cli->stop();
        while (fut.wait_for(std::chrono::milliseconds(500)) != std::future_status::ready);
        return cut;
    }
    auto res = fut.get();
    if (!res) {
        // The connection broke: the process may be ending right now (a crash closes the socket before the process object
        // is signalled), so give it a moment before deciding between gone and unreachable.
        plat::ProcInfo pi;
        if (t->watch) {
            plat::process_watch_query(t->watch, pi, 1500);
        } else {
            for (int i = 0; i < 15; ++i) {
                {
                    std::lock_guard<std::mutex> lk(g_mu);
                    if (finalize_locked(*t)) break;
                }
                plat::sleep_ms(100);
            }
        }
        std::lock_guard<std::mutex> lk(g_mu);
        if (finalize_locked(*t)) return ended_error(*t);
        double ago = std::chrono::duration<double>(Clock::now() - last_seen).count();
        return wrap_err("instance_unreachable", gone_message(instance, t->pid, ago, true));
    }
    try {
        json r = json::parse(res->body);
        if (r.contains("result")) {
            json out = r["result"];
            if (out.contains("structuredContent") && out["structuredContent"].is_object()) out["structuredContent"]["relayed_by"] = agent::config().instance_id;
            return out;
        }
        return wrap_err("relay_error", r.value("error", json::object()).value("message", "unknown error"));
    } catch (const std::exception &e) {
        return wrap_err("relay_error", std::string("invalid response: ") + e.what());
    }
}

// Member side: one heartbeat; returns true when the coordinator answered.
// State of this member's link to the coordinator, for status: "registered", "refused" (another token: the coordinator
// answers 401, so this game is not reachable through it), "foreign" (issue #5: the base port is held by a program that is
// not gmod-mcp) or "unreachable".
static std::mutex g_link_mu;
static std::string g_link = "unknown";
std::string link_state() {
    if (is_coordinator()) return "coordinator";
    std::lock_guard<std::mutex> lk(g_link_mu);
    return g_link;
}
static void set_link(const std::string &state, int status = 0) {
    std::lock_guard<std::mutex> lk(g_link_mu);
    if (state == g_link) return;
    g_link = state;
    if (state == "refused")
        logx::warn("the coordinator on port %d refused this instance (HTTP 401: it uses another token). This game is not "
                   "reachable through it: give every game the same gmod-mcp config folder (APPDATA on Windows, "
                   "XDG_CONFIG_HOME or ~/.config on Linux)", agent::config().base_port);
    else if (state == "foreign")
        logx::warn("port %d is held by another program: it answered HTTP %d and is not a gmod-mcp coordinator (this has nothing to do "
                   "with the token). This game is not reachable through that port: find the program (Get-NetTCPConnection "
                   "-LocalPort %d on Windows, ss -ltnp on Linux) and free the port, or set the GMOD_MCP_PORT environment "
                   "variable of the MCP client to this game's port (%d)",
                   agent::config().base_port, status, agent::config().base_port, agent::config().port);
}

static bool heartbeat() {
    auto cli = client(agent::config().base_port, 3);
    auto res = cli->Post("/internal/heartbeat", self_info().dump(), "application/json");
    // Who answered? gmod-mcp signs its answers (modules up to 0.3.6 send no header: JSON body); a 200 is a coordinator whatever its form.
    if (!res) set_link("unreachable");
    else if (res->status == 200) set_link("registered");
    else if (!gmcp::is_gmod_mcp_reply(res->has_header(gmcp::kSignatureHeader), res->body)) set_link("foreign", res->status);
    else if (res->status == 401 || res->status == 403) set_link("refused");
    else set_link("unreachable");
    if (res && res->status == 200) {
        try {
            json r = json::parse(res->body);
            if (r.contains("coordinator") && r["coordinator"].is_string()) {
                std::lock_guard<std::mutex> lk(g_cid_mu);
                g_coordinator_id = r["coordinator"].get<std::string>();
            }
        } catch (...) {
        }
        return true;
    }
    return false;
}

// Member side: the new coordinator asks to be heard now instead of at the next heartbeat.
static json on_reregister(const json &) {
    if (is_coordinator()) return json{{"ok", false}, {"error", "is_coordinator"}};
    return json{{"ok", heartbeat()}};
}

// Coordinator side, right after the take-over: the other instances listen on the next ports of the range (the main
// listener takes the first free port from the base one). Each is asked to register now; all in parallel.
static void solicit_members() {
    const int base = agent::config().base_port, own = agent::config().port;
    std::vector<std::thread> th;
    for (int p = base + 1; p < base + 16; ++p) {
        if (p == own) continue;
        th.emplace_back([p] {
            auto cli = client(p, 3);
            cli->set_connection_timeout(0, 300000);
            cli->Post("/internal/reregister", "{}", "application/json");
        });
    }
    for (auto &t : th) t.join();
}

void say_goodbye() {
    if (is_coordinator()) return;
    auto cli = client(agent::config().base_port, 1);
    cli->set_write_timeout(1, 0);
    json j{{"id", agent::config().instance_id}, {"port", agent::config().port}, {"closing", true}};
    cli->Post("/internal/heartbeat", j.dump(), "application/json");
}

static void loop() {
    double wait_s = kHeartbeatS;
    while (g_run) {
        for (int i = 0; i < (int)(wait_s * 10 + 0.5) && g_run && !g_wake; ++i) plat::sleep_ms(100);  // FP7137: ... && !g_wake
        g_wake = false;  // FP7137
        if (!g_run) break;
        {  // issue #8: this instance's own frame rate (the others are fed by their heartbeats)
            json me = self_info();
            std::lock_guard<std::mutex> lk(g_mu);
            double fps = me["fps"].is_number() ? me["fps"].get<double>() : -1.0;
            g_self_fps.feed(steady_s(), fps, me.value("loading", false) || me.value("last_tick_age_s", 0.0) >= health::kStalledShownS);
        }
        if (is_coordinator()) {
            wait_s = kHeartbeatS;
            std::lock_guard<std::mutex> lk(g_mu);
            purge_locked();
            continue;
        }
        if (heartbeat()) {
            wait_s = kHeartbeatS;
            continue;
        }
        wait_s = kFastRetryS;  // the coordinator may be gone: look again soon, do not wait 2 s
        { std::string l = link_state(); if (l == "foreign" || l == "refused") wait_s = kHeartbeatS; }  // the port is held: no fast retry
        // Try to take over the base port: it only succeeds when nobody holds it (exclusive bind), whatever the failure was.
        if (httpsrv::try_bind_extra(agent::config().base_port)) {
            g_takeover_ms = steady_ms();
            logx::info("coordinator unreachable: this instance is now the coordinator (port %d)", agent::config().base_port);
            wait_s = kHeartbeatS;
            solicit_members();
        }
    }
}

void start() {
    httpsrv::add_internal_route("heartbeat", on_heartbeat);
    httpsrv::add_internal_route("reregister", on_reregister);
    mcp::set_relay(relay);
    mcp::set_peers([] {
        std::vector<std::string> ids;
        if (!is_coordinator()) return ids;
        std::lock_guard<std::mutex> lk(g_mu);
        purge_locked();
        for (auto &kv : g_members) ids.push_back(kv.first);
        return ids;
    });
    if (!is_coordinator()) {
        if (heartbeat()) logx::info("registered with the coordinator on port %d", agent::config().base_port);
        else if (link_state() != "refused" && link_state() != "foreign") logx::warn("coordinator on port %d did not answer (will retry)", agent::config().base_port);
    }
    g_run = true;
    g_thread = std::thread(loop);
}

void stop() {
    g_run = false;
    if (g_thread.joinable()) g_thread.join();
}

}  // namespace coordinator

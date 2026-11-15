// FP7137 workaround (launch / map_load lock): pure decisions, expiry of stale entries, and the shared lock itself.
// Removed with the workaround, see docs/FP7137.md.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "bridge/fp7137.h"
#include "platform/platform.h"

using namespace fp7137;
using json = nlohmann::json;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

static void put_env(const char *k, const char *v) {
#ifdef _WIN32
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1);
    else unsetenv(k);
#endif
}
static double since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }
static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static int64_t wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

static Entry entry(char kind, uint32_t pid, int64_t until, const char *tag = "t") {
    Entry e;
    e.kind = kind;
    e.pid = pid;
    e.stamp = 0;
    e.until_ms = until;
    e.tag = tag;
    return e;
}
static std::string state_file(const std::string &dir) {
    std::string s;
    plat::read_file(dir + "/fp7137.state", s);
    return s;
}

static void test_pure() {
    // Round trip, and damaged lines are ignored.
    State s;
    s.last_start_ms = 1234;
    s.entries.push_back(entry('M', 42, 9999, "map:gm_construct"));
    s.entries.push_back(entry('L', 43, 8888, "launch"));
    State r = parse_state(serialize_state(s));
    CHECK(r.last_start_ms == 1234 && r.entries.size() == 2);
    CHECK(r.entries[0].kind == 'M' && r.entries[0].pid == 42 && r.entries[0].until_ms == 9999 && r.entries[0].tag == "map:gm_construct");
    State bad = parse_state("garbage\nE X 1 0 5 t\nE M 0 0 5 t\nE M 7 0 5\nS -4\nE M 9 0 77 ok\n\x01\x02\n");
    CHECK(bad.entries.size() == 1 && bad.entries[0].pid == 9 && bad.last_start_ms == 0);
    CHECK(parse_state("").entries.empty());
    Entry spaced = entry('M', 5, 10, "a b\tc");
    State sp;
    sp.entries.push_back(spaced);
    CHECK(parse_state(serialize_state(sp)).entries.size() == 1);  // a tag with spaces never breaks a line

    // Expiry: an entry past its deadline, or whose owner is gone, no longer counts; a live one stays.
    State p;
    p.entries = {entry('M', 1, 5000), entry('M', 2, 20000), entry('L', 3, 20000)};
    bool dropped = prune(p, 10000, [](const Entry &e) { return e.pid != 3; });
    CHECK(dropped && p.entries.size() == 1 && p.entries[0].pid == 2);
    CHECK(!prune(p, 10000, [](const Entry &) { return true; }));
    CHECK(p.entries.size() == 1);
    State edge;
    edge.entries = {entry('M', 1, 10000)};
    prune(edge, 10000, [](const Entry &) { return true; });
    CHECK(edge.entries.empty());  // until is exclusive: expired exactly at its deadline

    // launch waits while any map loads, never because of another launch or the settle delay.
    State none;
    CHECK(!launch_verdict(none, 1000).wait);
    State maps;
    maps.entries = {entry('M', 1, 99999, "map:a"), entry('M', 2, 99999, "map:b")};
    Verdict v = launch_verdict(maps, 1000);
    CHECK(v.wait && v.code == "map_load_in_progress" && v.reason.find("map:a") != std::string::npos && v.reason.find("map:b") != std::string::npos);
    State launches;
    launches.entries = {entry('L', 1, 99999)};
    launches.last_start_ms = 999;
    CHECK(!launch_verdict(launches, 1000).wait);

    // map_load waits while a launch runs and until settle after the last start; other map loads do not block it.
    CHECK(!map_load_verdict(none, 1000, 15000).wait);
    CHECK(!map_load_verdict(maps, 1000, 15000).wait);
    Verdict l = map_load_verdict(launches, 1000, 15000);
    CHECK(l.wait && l.code == "launch_in_progress" && l.ready_at_ms == 0);
    State started;
    started.last_start_ms = 100000;
    Verdict w = map_load_verdict(started, 100000 + 4000, 15000);
    CHECK(w.wait && w.code == "launch_in_progress" && w.ready_at_ms == 115000);
    CHECK(map_load_verdict(started, 114999, 15000).wait);
    CHECK(!map_load_verdict(started, 115000, 15000).wait);  // exactly settle ms after: free
    CHECK(!map_load_verdict(started, 500000, 15000).wait);
    CHECK(!map_load_verdict(started, 100000, 0).wait);      // a zero delay never waits

    // A wait whose known end lies beyond the deadline is refused at once; an unknown end is waited for.
    CHECK(exceeds_deadline(w, 110000));
    CHECK(!exceeds_deadline(w, 115000));
    CHECK(!exceeds_deadline(l, 1));  // end unknown
    CHECK(!exceeds_deadline(Verdict(), 0));
}

static std::vector<fs::path> g_dirs;
static std::string fresh_dir() {
    fs::path d = fs::temp_directory_path() / ("gmcp-fp7137-" + std::to_string(plat::process_id()) + "-" + std::to_string(wall_ms()));
    fs::create_directories(d);
    g_dirs.push_back(d);
    set_dir(d.generic_string());
    return d.generic_string();
}

static void test_gate() {
    put_env("GMOD_MCP_FP7137_WORKAROUND", nullptr);
    put_env("GMOD_MCP_FP7137_SETTLE_MS", "700");
    std::string dir = fresh_dir();
    std::atomic<bool> no{false};
    CHECK(enabled() && settle_ms() == 700);

    // Free: no wait, the slot is registered with the owner, and released on destruction.
    {
        Gate g;
        g.enter(Kind::MapLoad, "map:a", 5, 30, &no, nullptr);
        CHECK(g.ok() && g.waited_s() == 0);
        State s = parse_state(state_file(dir));
        CHECK(s.entries.size() == 1 && s.entries[0].kind == 'M' && s.entries[0].pid == plat::process_id());
    }
    CHECK(parse_state(state_file(dir)).entries.empty());

    // Two map loads do not wait for each other.
    {
        Gate a, b;
        a.enter(Kind::MapLoad, "map:a", 5, 30, &no, nullptr);
        auto t0 = Clock::now();
        b.enter(Kind::MapLoad, "map:b", 5, 30, &no, nullptr);
        CHECK(b.ok() && since(t0) < 0.3 && b.waited_s() == 0);
    }

    // A launch waits for a running map load (and says why, through the notify callback too).
    {
        Gate map;
        map.enter(Kind::MapLoad, "map:slow", 5, 30, &no, nullptr);
        std::thread rel([&] { sleep_ms(500); map.release(); });
        Gate l;
        int notes = 0;
        std::string last;
        l.enter(Kind::Launch, "launch", 5, 30, &no, [&](double, const std::string &r) { ++notes; last = r; });
        rel.join();
        CHECK(l.ok() && l.waited_s() >= 0.4 && l.waited_s() < 3 && notes >= 1 && last.find("map:slow") != std::string::npos);
        CHECK(l.reason().find("map:slow") != std::string::npos);
    }

    // A map load waits for a running launch.
    {
        Gate l;
        l.enter(Kind::Launch, "launch", 5, 30, &no, nullptr);
        std::thread rel([&] { sleep_ms(400); l.release(); });
        Gate m;
        m.enter(Kind::MapLoad, "map:a", 5, 30, &no, nullptr);
        rel.join();
        CHECK(m.ok() && m.waited_s() >= 0.3 && m.reason().find("launch") != std::string::npos);
    }

    // A wait that cannot end within the budget fails cleanly, nothing is registered.
    {
        Gate map;
        map.enter(Kind::MapLoad, "map:slow", 5, 30, &no, nullptr);
        Gate l;
        auto t0 = Clock::now();
        l.enter(Kind::Launch, "launch", 0.5, 30, &no, nullptr);
        double took = since(t0);
        CHECK(!l.ok() && !l.aborted() && l.code() == "map_load_in_progress" && took >= 0.4 && took < 2);
        json r = l.error_result();
        CHECK(r["isError"] == true && r["structuredContent"]["error"]["code"] == "map_load_in_progress" && r["structuredContent"]["waited_s"].get<double>() >= 0.4);
        CHECK(r["content"][0]["text"].get<std::string>().find("map_load_in_progress") != std::string::npos);
        CHECK(parse_state(state_file(dir)).entries.size() == 1);  // only the map load's own entry
    }

    // After a game start, a map load waits for the settle delay; when the delay exceeds the budget it fails at once.
    {
        Gate l;
        l.enter(Kind::Launch, "launch", 5, 30, &no, nullptr);
        l.note_started();
        l.release();
        Gate quick;
        auto t0 = Clock::now();
        quick.enter(Kind::MapLoad, "map:a", 0.2, 30, &no, nullptr);
        CHECK(!quick.ok() && quick.code() == "launch_in_progress" && since(t0) < 0.15);  // known end beyond the budget: no polling
        Gate m;
        m.enter(Kind::MapLoad, "map:a", 5, 30, &no, nullptr);
        CHECK(m.ok() && m.waited_s() >= 0.4 && m.waited_s() < 2);
        Gate after;
        auto t1 = Clock::now();
        after.enter(Kind::MapLoad, "map:b", 5, 30, &no, nullptr);
        CHECK(after.ok() && since(t1) < 0.3);  // the delay has elapsed
    }

    // Cancelled while waiting.
    {
        Gate l;
        l.enter(Kind::Launch, "launch", 5, 30, &no, nullptr);
        std::atomic<bool> cancel{false};
        std::thread c([&] { sleep_ms(300); cancel = true; });
        Gate m;
        m.enter(Kind::MapLoad, "map:a", 5, 30, &cancel, nullptr);
        c.join();
        CHECK(!m.ok() && m.aborted() && m.waited_s() >= 0.25 && m.waited_s() < 2);
    }

    // The slot is released by a background poll when the task ends, and at its expiry at the latest.
    {
        std::atomic<bool> running{true};
        {
            Gate m;
            m.enter(Kind::MapLoad, "map:task", 5, 30, &no, nullptr);
            m.release_when_done([&] { return running.load(); });
        }  // the gate object is gone, the slot stays
        CHECK(parse_state(state_file(dir)).entries.size() == 1);
        Gate blocked;
        blocked.enter(Kind::Launch, "launch", 0.3, 30, &no, nullptr);
        CHECK(!blocked.ok());
        running = false;
        Gate freed;
        auto t0 = Clock::now();
        freed.enter(Kind::Launch, "launch", 5, 30, &no, nullptr);
        CHECK(freed.ok() && since(t0) < 3);
    }
    {
        Gate m;
        m.enter(Kind::MapLoad, "map:short", 5, 0.4, &no, nullptr);  // held 0.4 s if nobody releases it
        m.release_when_done([] { return true; });
        Gate l;
        l.enter(Kind::Launch, "launch", 5, 30, &no, nullptr);
        CHECK(l.ok() && l.waited_s() >= 0.3 && l.waited_s() < 3);
    }
    sleep_ms(1500);  // let the detached pollers finish before the folder goes
}

static void test_stale_and_switch() {
    put_env("GMOD_MCP_FP7137_SETTLE_MS", "700");
    std::string dir = fresh_dir();
    std::atomic<bool> no{false};

    // A bridge that died while its map load ran: a pid that does not exist, a far-away expiry. No deadlock.
    State s;
    s.entries.push_back(entry('M', 0x7ffffff0u, wall_ms() + 3600 * 1000, "map:ghost"));
    s.entries.push_back(entry('L', 0x7fffffe0u, wall_ms() + 3600 * 1000, "launch"));
    plat::write_file(dir + "/fp7137.state", serialize_state(s));
    {
        Gate l;
        auto t0 = Clock::now();
        l.enter(Kind::Launch, "launch", 5, 30, &no, nullptr);
        CHECK(l.ok() && l.waited_s() == 0 && since(t0) < 0.5);
    }
    {
        Gate m;
        m.enter(Kind::MapLoad, "map:a", 5, 30, &no, nullptr);
        CHECK(m.ok() && m.waited_s() == 0);
        CHECK(state_file(dir).find("ghost") == std::string::npos);  // the dead owner's entries were removed
    }

    // An entry of a live process (this one) that is past its expiry does not count either.
    State e;
    e.entries.push_back(entry('M', plat::process_id(), wall_ms() - 1000, "map:expired"));
    plat::write_file(dir + "/fp7137.state", serialize_state(e));
    {
        Gate l;
        l.enter(Kind::Launch, "launch", 5, 30, &no, nullptr);
        CHECK(l.ok() && l.waited_s() == 0);
    }

    // A corrupt state file is an empty state.
    plat::write_file(dir + "/fp7137.state", "\x01\x02 not a state \xff\n");
    {
        Gate l;
        l.enter(Kind::Launch, "launch", 5, 30, &no, nullptr);
        CHECK(l.ok());
    }

    // Kill switch: nothing waits, nothing is written, nothing is released.
    State busy;
    busy.entries.push_back(entry('M', plat::process_id(), wall_ms() + 60000, "map:busy"));
    busy.last_start_ms = wall_ms();
    plat::write_file(dir + "/fp7137.state", serialize_state(busy));
    put_env("GMOD_MCP_FP7137_WORKAROUND", "0");
    CHECK(!enabled());
    {
        Gate l, m;
        auto t0 = Clock::now();
        l.enter(Kind::Launch, "launch", 5, 30, &no, nullptr);
        m.enter(Kind::MapLoad, "map:a", 5, 30, &no, nullptr);
        l.note_started();
        CHECK(l.ok() && m.ok() && l.waited_s() == 0 && m.waited_s() == 0 && since(t0) < 0.3);
    }
    CHECK(parse_state(state_file(dir)).entries.size() == 1);  // the busy entry is untouched
    put_env("GMOD_MCP_FP7137_WORKAROUND", "1");
    CHECK(enabled());
    put_env("GMOD_MCP_FP7137_WORKAROUND", nullptr);
    CHECK(enabled());

    // annotate: the wait is reported in the structured result and in its text twin; no wait, no noise.
    put_env("GMOD_MCP_FP7137_SETTLE_MS", "400");
    {
        Gate l;
        l.enter(Kind::Launch, "launch", 5, 30, &no, nullptr);
        l.note_started();
        l.release();
        Gate m;
        m.enter(Kind::MapLoad, "map:a", 5, 30, &no, nullptr);
        json res{{"content", json::array({json{{"type", "text"}, {"text", "{}"}}})}, {"structuredContent", {{"map", "a"}}}, {"isError", false}};
        m.annotate(res);
        CHECK(res["structuredContent"]["waited_s"].get<double>() > 0.2 && res["structuredContent"]["waited_reason"].is_string() && res["structuredContent"]["map"] == "a");
        CHECK(json::parse(res["content"][0]["text"].get<std::string>()) == res["structuredContent"]);
        Gate free;
        json plain{{"content", json::array({json{{"type", "text"}, {"text", "{}"}}})}, {"structuredContent", {{"map", "a"}}}, {"isError", false}};
        json copy = plain;
        free.enter(Kind::MapLoad, "map:a", 5, 30, &no, nullptr);
        free.annotate(plain);
        CHECK(plain == copy);
    }
}

static void test_stagger() {
    CHECK(parallel_limit(0, true) == 0 && parallel_limit(1, true) == 1 && parallel_limit(2, true) == 2);
    CHECK(parallel_limit(3, true) == 2 && parallel_limit(8, true) == 2);  // at most 2 maps at a time
    CHECK(parallel_limit(8, false) == 8 && parallel_limit(0, false) == 0);  // workaround off: all at once

    auto run = [](size_t n, std::vector<int> &seen, int &peak) {
        std::atomic<int> now{0}, mx{0};
        std::mutex mu;
        for_each_limited(n, [&](size_t i) {
            int c = ++now;
            int m = mx.load();
            while (c > m && !mx.compare_exchange_weak(m, c)) {}
            sleep_ms(80);
            --now;
            std::lock_guard<std::mutex> lk(mu);
            seen.push_back((int)i);
        });
        peak = mx.load();
    };
    put_env("GMOD_MCP_FP7137_WORKAROUND", nullptr);
    std::vector<int> seen;
    int peak = 0;
    run(7, seen, peak);
    std::sort(seen.begin(), seen.end());
    CHECK(peak == 2 && seen == (std::vector<int>{0, 1, 2, 3, 4, 5, 6}));  // every item once, 2 at a time
    seen.clear();
    run(1, seen, peak);
    CHECK(peak == 1 && seen == (std::vector<int>{0}));
    seen.clear();
    run(0, seen, peak);
    CHECK(peak == 0 && seen.empty());
    put_env("GMOD_MCP_FP7137_WORKAROUND", "0");
    seen.clear();
    run(5, seen, peak);
    CHECK(peak == 5 && seen.size() == 5);  // kill switch: all at once, as before
    put_env("GMOD_MCP_FP7137_WORKAROUND", nullptr);
}

void test_fp7137() {
    test_pure();
    test_gate();
    test_stale_and_switch();
    test_stagger();
    put_env("GMOD_MCP_FP7137_SETTLE_MS", nullptr);
    for (const auto &d : g_dirs) {
        std::error_code ec;
        fs::remove_all(d, ec);
    }
}

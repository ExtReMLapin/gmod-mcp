#include "events.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include "console.h"
#include "jobs.h"

using nlohmann::json;

namespace events {

struct Ev {
    uint64_t seq;
    double time;
    std::string type;
    json data;
};

static std::mutex g_mu;
static std::condition_variable g_cv;
static std::deque<Ev> g_events;
static uint64_t g_next = 0;
static const size_t kCap = 5000;

struct Channel {
    std::deque<json> items;  // each item carries "seq"
    uint64_t next = 0;
    uint64_t total = 0;
};
static std::map<std::string, Channel> g_channels;
static const size_t kChanCap = 10000;

void publish(const std::string &type, const json &data) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_events.push_back(Ev{g_next++, console::uptime(), type, data});
        while (g_events.size() > kCap) g_events.pop_front();
    }
    g_cv.notify_all();
}

uint64_t cursor_now() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_next;
}

static bool matches(const std::vector<std::string> &types, const std::string &t) {
    if (types.empty()) return true;
    for (const auto &x : types)
        if (x == t) return true;
    return false;
}

json wait(const std::vector<std::string> &types, uint64_t cursor, double timeout_s, size_t limit) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                           std::chrono::duration<double>(timeout_s));
    std::unique_lock<std::mutex> lk(g_mu);
    uint64_t lost = 0;
    for (;;) {
        uint64_t oldest = g_events.empty() ? g_next : g_events.front().seq;
        if (cursor > g_next) cursor = g_next;
        if (cursor < oldest) {
            lost += oldest - cursor;
            cursor = oldest;
        }
        json out = json::array();
        uint64_t next = cursor;
        for (const auto &e : g_events) {
            if (e.seq < cursor) continue;
            next = e.seq + 1;
            if (!matches(types, e.type)) continue;
            out.push_back(json{{"seq", e.seq}, {"time", e.time}, {"type", e.type}, {"data", e.data}});
            if (out.size() >= limit) break;
        }
        if (out.size() < limit) next = g_next;
        if (!out.empty()) return json{{"events", out}, {"next_cursor", next}, {"lost", lost}, {"timed_out", false}};
        cursor = next;  // nothing of interest: advance and wait
        if (g_cv.wait_until(lk, deadline) == std::cv_status::timeout) {
            return json{{"events", json::array()}, {"next_cursor", g_next}, {"lost", lost}, {"timed_out", true}};
        }
    }
}

void report(const std::string &channel, const std::string &realm, const json &value) {
    uint64_t seq;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        Channel &c = g_channels[channel];
        seq = c.next++;
        c.total++;
        c.items.push_back(json{{"seq", seq}, {"time", console::uptime()}, {"realm", realm}, {"value", value}});
        while (c.items.size() > kChanCap) c.items.pop_front();
    }
    publish("report", json{{"channel", channel}, {"seq", seq}});
}

json reports_read(const std::string &channel, uint64_t cursor, size_t limit, double wait_s) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                           std::chrono::duration<double>(wait_s));
    std::unique_lock<std::mutex> lk(g_mu);
    for (;;) {
        auto it = g_channels.find(channel);
        if (it != g_channels.end()) {
            Channel &c = it->second;
            uint64_t oldest = c.items.empty() ? c.next : c.items.front()["seq"].get<uint64_t>();
            uint64_t lost = cursor < oldest ? oldest - cursor : 0;
            uint64_t from = cursor < oldest ? oldest : cursor;
            if (from < c.next) {
                json out = json::array();
                uint64_t next = from;
                for (const auto &x : c.items) {
                    uint64_t s = x["seq"].get<uint64_t>();
                    if (s < from) continue;
                    if (out.size() >= limit) break;
                    out.push_back(x);
                    next = s + 1;
                }
                return json{{"channel", channel}, {"items", out}, {"next_cursor", next}, {"lost", lost}, {"timed_out", false}};
            }
        }
        if (wait_s <= 0 || g_cv.wait_until(lk, deadline) == std::cv_status::timeout) {
            uint64_t next = it != g_channels.end() ? it->second.next : cursor;
            return json{{"channel", channel}, {"items", json::array()}, {"next_cursor", std::max(cursor, next)}, {"lost", 0},
                        {"timed_out", wait_s > 0}};
        }
    }
}

json reports_channels() {
    std::lock_guard<std::mutex> lk(g_mu);
    json out = json::array();
    for (const auto &kv : g_channels)
        out.push_back(json{{"channel", kv.first}, {"buffered", kv.second.items.size()}, {"total", kv.second.total},
                           {"next_cursor", kv.second.next}});
    return out;
}

void reports_clear(const std::string &channel) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (channel.empty()) g_channels.clear();
    else g_channels.erase(channel);
}

// --- Watchdog ---
static std::atomic<bool> g_wd_run{false};
static std::thread g_wd;

void start_watchdog(double stall_s) {
    if (g_wd_run) return;
    g_wd_run = true;
    g_wd = std::thread([stall_s] {
        bool stalled = false;
        while (g_wd_run) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            double since = jobs::seconds_since_last_frame();
            if (since < 0) continue;
            if (!stalled && since > stall_s) {
                stalled = true;
                publish("main_thread_stalled", json{{"seconds_since_last_frame", since}});
            } else if (stalled && since < 1.0) {
                stalled = false;
                publish("main_thread_resumed", json::object());
            }
        }
    });
}

void stop_watchdog() {
    g_wd_run = false;
    if (g_wd.joinable()) g_wd.join();
}

}  // namespace events

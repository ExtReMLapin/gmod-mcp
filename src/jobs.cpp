#include "jobs.h"

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>

using nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace jobs {

struct Pending {
    Fn fn;
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    bool started = false;
    bool cancelled = false;
    json result;
};

static std::mutex g_mu;
static std::deque<std::shared_ptr<Pending>> g_queue;

static std::atomic<uint64_t> g_frames{0};
static std::atomic<int64_t> g_last_frame_ns{0};
static std::mutex g_fmu;
static double g_ft[256] = {0};
static size_t g_ft_n = 0, g_ft_i = 0;
static Clock::time_point g_prev_frame{};

json run_on_main(Fn fn, double timeout_s) {
    auto p = std::make_shared<Pending>();
    p->fn = std::move(fn);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_queue.push_back(p);
    }
    std::unique_lock<std::mutex> lk(p->m);
    bool ok = p->cv.wait_for(lk, std::chrono::duration<double>(timeout_s), [&] { return p->done; });
    if (!ok) {
        if (!p->started) p->cancelled = true;
        return json{{"__timeout", true}, {"started", p->started}};
    }
    return p->result;
}

bool is_timeout(const json &r) { return r.is_object() && r.contains("__timeout"); }

void pump(double max_ms) {
    auto t0 = Clock::now();
    for (;;) {
        std::shared_ptr<Pending> p;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (g_queue.empty()) return;
            p = g_queue.front();
            g_queue.pop_front();
        }
        {
            std::lock_guard<std::mutex> lk(p->m);
            if (p->cancelled) continue;
            p->started = true;
        }
        json r;
        try {
            r = p->fn();
        } catch (const std::exception &e) {
            r = json{{"__error", std::string("C++ exception: ") + e.what()}};
        }
        {
            std::lock_guard<std::mutex> lk(p->m);
            p->result = std::move(r);
            p->done = true;
        }
        p->cv.notify_all();
        if (std::chrono::duration<double, std::milli>(Clock::now() - t0).count() > max_ms) return;
    }
}

size_t pending() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_queue.size();
}

void cancel_all() {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto &p : g_queue) {
        std::lock_guard<std::mutex> lk2(p->m);
        p->cancelled = true;
        p->result = json{{"__error", "module stopped"}};
        p->done = true;
        p->cv.notify_all();
    }
    g_queue.clear();
}

void on_frame() {
    auto now = Clock::now();
    g_frames++;
    g_last_frame_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    std::lock_guard<std::mutex> lk(g_fmu);
    if (g_prev_frame.time_since_epoch().count() != 0) {
        g_ft[g_ft_i] = std::chrono::duration<double>(now - g_prev_frame).count();
        g_ft_i = (g_ft_i + 1) % 256;
        if (g_ft_n < 256) g_ft_n++;
    }
    g_prev_frame = now;
}

uint64_t frame_count() { return g_frames.load(); }

double seconds_since_last_frame() {
    int64_t last = g_last_frame_ns.load();
    if (last == 0) return -1;
    auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    return (double)(now - last) / 1e9;
}

json frame_stats() {
    std::lock_guard<std::mutex> lk(g_fmu);
    if (g_ft_n == 0) return json{{"fps", nullptr}, {"frametime_avg_ms", nullptr}, {"frametime_max_ms", nullptr}, {"samples", 0}};
    double sum = 0, mx = 0;
    for (size_t i = 0; i < g_ft_n; ++i) {
        sum += g_ft[i];
        if (g_ft[i] > mx) mx = g_ft[i];
    }
    double avg = sum / (double)g_ft_n;
    return json{{"fps", avg > 0 ? 1.0 / avg : 0.0}, {"frametime_avg_ms", avg * 1000.0}, {"frametime_max_ms", mx * 1000.0},
                {"samples", g_ft_n}};
}

}  // namespace jobs

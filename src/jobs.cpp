#include "jobs.h"

#include "health.h"

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
// Times (seconds, steady clock) at which the last frames ended: the frame rate is counted over a time window (health.h), so
// the history must reach back kFpsWindowS seconds even at 1000 fps.
static const size_t kFrameRing = 4096;
static double g_ft[kFrameRing] = {0};
static size_t g_ft_n = 0, g_ft_i = 0;

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

static double steady_s() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

void on_frame_at(double t_s) {
    std::lock_guard<std::mutex> lk(g_fmu);
    g_ft[g_ft_i] = t_s;
    g_ft_i = (g_ft_i + 1) % kFrameRing;
    if (g_ft_n < kFrameRing) g_ft_n++;
}

void on_frame() {
    auto now = Clock::now();
    g_frames++;
    g_last_frame_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    on_frame_at(std::chrono::duration<double>(now.time_since_epoch()).count());
}

uint64_t frame_count() { return g_frames.load(); }

double seconds_since_last_frame() {
    int64_t last = g_last_frame_ns.load();
    if (last == 0) return -1;
    auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    return (double)(now - last) / 1e9;
}

json frame_stats_at(double now_s) {
    std::vector<double> ts;  // oldest first
    {
        std::lock_guard<std::mutex> lk(g_fmu);
        ts.reserve(g_ft_n);
        size_t first = g_ft_n < kFrameRing ? 0 : g_ft_i;
        for (size_t k = 0; k < g_ft_n; ++k) ts.push_back(g_ft[(first + k) % kFrameRing]);
    }
    // Frame times of the last 256 frames: average and worst.
    double sum = 0, mx = 0;
    size_t samples = 0;
    for (size_t k = ts.size() > 257 ? ts.size() - 256 : 1; k < ts.size(); ++k) {
        double d = ts[k] - ts[k - 1];
        sum += d;
        if (d > mx) mx = d;
        ++samples;
    }
    if (samples == 0) return json{{"fps", nullptr}, {"frametime_avg_ms", nullptr}, {"frametime_max_ms", nullptr}, {"samples", 0}};
    double fps = health::fps_over_window(ts, now_s, health::kFpsWindowS);
    double avg = sum / (double)samples;
    return json{{"fps", fps >= 0 ? json(fps) : json(nullptr)}, {"frametime_avg_ms", avg * 1000.0}, {"frametime_max_ms", mx * 1000.0},
                {"samples", samples}, {"fps_window_s", health::kFpsWindowS}};
}

json frame_stats() { return frame_stats_at(steady_s()); }

}  // namespace jobs

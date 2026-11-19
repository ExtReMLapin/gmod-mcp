#include "health.h"

#include <algorithm>

namespace health {

double fps_over_window(const std::vector<double> &frame_times, double now, double window_s) {
    const size_t n = frame_times.size();
    if (n < 2 || window_s <= 0) return -1;
    const double cutoff = now - window_s;
    // First frame inside the window.
    size_t i = (size_t)(std::upper_bound(frame_times.begin(), frame_times.end(), cutoff) - frame_times.begin());
    double from;
    size_t frames;
    if (i == 0) {  // the history is shorter than the window: count the intervals it holds
        from = frame_times[0];
        frames = n - 1;
    } else {
        from = cutoff;
        frames = n - i;
    }
    double elapsed = now - from;
    if (elapsed < 0.001) return -1;
    return (double)frames / elapsed;
}

Assessment assess(double last_tick_age_s, double heartbeat_age_s, bool loading, double hung_after_s, bool silent) {
    Assessment a;
    a.hung_after_s = hung_after_s > 0 ? hung_after_s : (loading ? kHungAfterLoadingS : kHungAfterS);
    if (last_tick_age_s < 0) last_tick_age_s = 0;
    if (heartbeat_age_s < 0) heartbeat_age_s = 0;
    // The game says its main thread is blocked, or its heartbeats stopped (nobody can tell more): both add up, the heartbeat
    // is that much older than the figure it carried.
    if (last_tick_age_s >= kStalledShownS || heartbeat_age_s > kHeartbeatMissedS || silent) a.stalled_s = last_tick_age_s + heartbeat_age_s;
    a.hung = a.stalled_s >= a.hung_after_s;
    return a;
}

void FpsWatch::feed(double now_s, double fps, bool excluded) {
    if (last_ >= 0 && now_s - last_ > 3 * 2.0 + 0.5) since_ = -1;  // samples are 2 s apart: a gap means some were missed
    last_ = now_s;
    if (excluded || fps < 0 || fps >= kLowFps) {
        since_ = -1;
        return;
    }
    if (since_ < 0) since_ = now_s;
}

double FpsWatch::low_s(double now_s, double shown_s) const {
    if (since_ < 0) return 0;
    double d = now_s - since_;
    return d >= shown_s ? d : 0;
}

}  // namespace health

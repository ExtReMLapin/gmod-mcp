// Health of a game as the coordinator can tell it from what the game sends (issue #8): the frame rate over a recent time
// window, how long the main thread has been blocked, when that makes the game "hung", and for how long the frame rate
// stayed low. Pure functions and small state machines: tested without a game.
#pragma once

#include <vector>

namespace health {

// The frame rate is counted over this many seconds, not over a number of frames: with a count (the last 256 frames) a client
// that went through a slow stretch (a map load at 2 fps) kept reporting that rate for minutes after it ran fine again.
constexpr double kFpsWindowS = 3.0;

// Frames per second over the last `window_s` seconds before `now`. `frame_times` are the times (seconds, same clock as `now`)
// at which frames ended, oldest first. A stall counts: nothing for the whole window gives 0. Returns -1 when it cannot be
// told (fewer than two frames known, or no time elapsed).
double fps_over_window(const std::vector<double> &frame_times, double now, double window_s);

// Main thread blocked for at least this long (seconds) and the game is no longer "slow" but "hung": a map load that never
// ends, an infinite loop, a crash dialog. Beyond what a long but healthy load takes (the launch tool waits 180 s for a client).
constexpr double kHungAfterS = 60.0;
// The same while a map load is in progress (the `loading` flag): the biggest maps stall the main thread for minutes.
constexpr double kHungAfterLoadingS = 180.0;
// Below this a stalled main thread is not worth a figure (a normal hitch).
constexpr double kStalledShownS = 2.0;
// A heartbeat is sent every 2 s: one that is older than this has been missed.
constexpr double kHeartbeatMissedS = 5.0;

struct Assessment {
    double stalled_s = 0;   // seconds the main thread has been blocked (0 = it ticks)
    bool hung = false;
    double hung_after_s = 0;  // the limit that applied
};
// `last_tick_age_s`: the game's own measure at the time of its last heartbeat (seconds since its main thread last ran);
// `heartbeat_age_s`: how old that heartbeat is. When the heartbeats stopped altogether (the whole process is frozen) the
// silence itself counts as stalled time.
// `hung_after_s` > 0 overrides the limit (tests); `silent`: the caller already knows the heartbeats stopped (the instance left the
// list for that reason), whatever their age.
Assessment assess(double last_tick_age_s, double heartbeat_age_s, bool loading, double hung_after_s = 0, bool silent = false);

// A frame rate under this (a light client is capped at 20) for kLowFpsShownS seconds is reported as `fps_low_s`.
constexpr double kLowFps = 10.0;
constexpr double kLowFpsShownS = 6.0;

// How long the frame rate has stayed under kLowFps. Fed with each heartbeat's figure. Samples taken while the game loads or
// is stalled do not count (those have their own flags), nor does an unknown figure; a gap in the samples starts over.
class FpsWatch {
public:
    void feed(double now_s, double fps, bool excluded);
    // Seconds under the threshold when at least `shown_s`, else 0.
    double low_s(double now_s, double shown_s = kLowFpsShownS) const;

private:
    double since_ = -1;  // start of the current low stretch, -1 = not low
    double last_ = -1;   // time of the last sample
};

}  // namespace health

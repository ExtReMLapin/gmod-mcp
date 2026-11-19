// FP7137 workaround (issue #7): the module's "a map is loading" flag, sent with every heartbeat so that the bridge can
// hold back instances_launch while any game loads a map, whatever started the load (map, changelevel, Lua, a script
// talking HTTP to the game, map_load). REMOVABLE: see docs/FP7137.md.
//
// Two sources, each on from the moment the module learns of a load until it learns that it ended:
//   Server: a new server Lua state appeared (a map load starts) -> map_loaded was published;
//   Menu:   the menu state's IsInLoading() (the loading screen is up), polled by the pump.
// A source that never reports its end (a load that failed, a frozen game) stops counting after kMaxLoadS, so the flag
// can never block launches for ever. Pure logic, unit tested (tests/native/test_fp7137.cpp).
#pragma once

namespace loadflag {

constexpr double kMaxLoadS = 300.0;  // FP7137: a source on for longer than this no longer counts

class Tracker {
  public:
    enum Source { Server = 0, Menu = 1, kSources = 2 };

    // Sets a source on or off at time now_s. A source that is already on keeps its start time (a poll saying "still
    // loading" must not hide a stuck load), unless restart is true (a new load has begun). Returns true when the
    // overall flag changed.
    bool set(Source s, bool on, double now_s, bool restart = false) {
        bool before = active(now_s);
        if (on) {
            if (!on_[s] || restart) since_[s] = now_s;
            on_[s] = true;
        } else {
            on_[s] = false;
        }
        return before != active(now_s);
    }
    bool active(double now_s) const {
        for (int i = 0; i < kSources; ++i)
            if (counts(i, now_s)) return true;
        return false;
    }
    // Age of the oldest source still counting (0 when none).
    double since_s(double now_s) const {
        double best = 0;
        for (int i = 0; i < kSources; ++i)
            if (counts(i, now_s) && now_s - since_[i] > best) best = now_s - since_[i];
        return best;
    }

  private:
    bool counts(int i, double now_s) const { return on_[i] && now_s - since_[i] <= kMaxLoadS; }
    bool on_[kSources] = {false, false};
    double since_[kSources] = {0, 0};
};

}  // namespace loadflag

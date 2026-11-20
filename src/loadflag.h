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

#include <algorithm>

namespace loadflag {

constexpr double kMaxLoadS = 300.0;  // FP7137: a source on for longer than this no longer counts
constexpr double kServerOnlyMaxS = 30.0;  // issue #8: the server source alone (the menu polls "not loading") counts this long at most
constexpr double kStateGoneGraceS = 1.0;  // issue #8: the server source is dropped once no server state has existed for this long

class Tracker {
  public:
    enum Source { Server = 0, Menu = 1, kSources = 2 };

    // Sets a source on or off at time now_s. A source that is already on keeps its start time (a poll saying "still
    // loading" must not hide a stuck load), unless restart is true (a new load has begun). Returns true when the
    // overall flag changed.
    bool set(Source s, bool on, double now_s, bool restart = false) {
        bool before = active(now_s);
        if (s == Menu) {  // issue #8: when the menu started to say "not loading" (the loading screen is down)
            menu_polled_ = true;
            if (on) menu_off_since_ = -1;
            else {
                if (menu_off_since_ < 0) menu_off_since_ = now_s;
                menu_last_off_ = now_s;
            }
        }
        if (on) {
            if (!on_[s] || restart) {
                since_[s] = now_s;
                if (s == Server) gone_since_ = -1;
            }
            on_[s] = true;
        } else {
            on_[s] = false;
        }
        return before != active(now_s);
    }
    // Issue #8: tells whether a server Lua state exists right now (fed every frame). The server source is switched on when a
    // state opens and off by map_loaded or by the pump seeing the realms disappear; a state that opened and closed between two
    // pump runs (`map x` then `disconnect` at once) was never seen by either, and the source stayed on. A server source with no
    // state for kStateGoneGraceS in a row is therefore dropped. Returns true when the overall flag changed.
    bool note_server_state(bool present, double now_s) {
        if (!on_[Server] || present) {
            gone_since_ = -1;
            return false;
        }
        if (gone_since_ < 0) {
            gone_since_ = now_s;
            return false;
        }
        if (now_s - gone_since_ < kStateGoneGraceS) return false;
        gone_since_ = -1;
        return set(Server, false, now_s);
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
    bool counts(int i, double now_s) const {
        if (!on_[i] || now_s - since_[i] > kMaxLoadS) return false;
        // Issue #8: the server source alone (the menu is polled and says the loading screen is down) is a load that should end
        // within moments (15 settle frames, then map_loaded): bounded well under kMaxLoadS, so that a load that never reaches
        // map_loaded (a client that never joined, a disconnect) cannot hold launches back for minutes.
        // The time counted is the span the polls actually saw (a blocked main thread does not poll: a long blocking load is not cut).
        if (i == Server && menu_polled_ && menu_off_since_ >= 0 && menu_last_off_ - std::max(since_[Server], menu_off_since_) > kServerOnlyMaxS) return false;
        return true;
    }
    bool on_[kSources] = {false, false};
    double since_[kSources] = {0, 0};
    bool menu_polled_ = false;     // issue #8
    double menu_off_since_ = -1;   // issue #8: -1 = the menu says loading, or was never polled
    double menu_last_off_ = -1;    // issue #8: the last poll that said "not loading"
    double gone_since_ = -1;       // issue #8: since when no server state exists while the server source is on
};

}  // namespace loadflag

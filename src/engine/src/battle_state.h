#pragma once
// Whether a fight is on, from the two facts the game gives (docs/engine-module.md, "Combat"):
//
// - the battle signal ([first_person] battle_signal, by default
//   EndBattleAPI.GetBattleSceneID): it turns on when the first fight of a battle area starts
//   and stays on for as long as the player is in that area, through the walks, shops and
//   menus between fights, so it is a gate, not the fight;
// - live battle objects ([first_person] battle_objects): the battle HUD's target marker
//   (BattleTarget_New_C) shown on screen. It is shown for the whole fight, hidden between
//   fights, with gaps of up to about 2.3 s mid-fight while the target changes.
//
// A fight starts when the signal turns on (that is the start of the first fight in an area),
// or when the objects are seen for `enter_polls` polls in a row while the signal is on. It
// ends when the signal turns off, or when the objects have not been seen for `exit_seconds`
// (after `rise_grace_seconds` from a start by the signal, while the object scan finds the
// marker of a new fight).
//
// Pure logic without the game, so that it can be tested and replayed on recorded logs
// (src/engine/tests/engine_tests.cpp).

#include <string>

namespace ff7vr::engine::battle {

struct Config {
    bool objects = true;              // [first_person] battle_objects: 0 = the signal alone decides
    int enter_polls = 2;              // consecutive polls with objects shown before a fight starts
    double exit_seconds = 3.0;        // [first_person] battle_exit_seconds: no objects this long ends a fight
    double rise_grace_seconds = 8.0;  // after a start by the signal, how long a fight holds without objects
};

struct Input {
    double t = 0.0;    // seconds
    int signal = -1;   // battle signal: -1 not available, 0 off, 1 on
    int objects = -1;  // live battle objects: -1 not known yet, 0 none shown, 1 shown
};

struct Change {
    bool changed = false;
    bool fight = false;
    std::string reason;  // which condition changed (for the log)
};

class Decider {
public:
    Change update(const Config& c, const Input& in) {
        Change r;
        const bool was = fight_;
        const int prev_signal = signal_;
        signal_ = in.signal;
        if (in.objects == 1) {
            ++shown_polls_;
            last_shown_ = in.t;
        } else {
            shown_polls_ = 0;
        }
        if (!c.objects) {
            fight_ = in.signal == 1;
            if (fight_ != was) r.reason = std::string("battle signal ") + (fight_ ? "on" : "off");
        } else if (in.signal != 1) {
            fight_ = false;
            if (was) r.reason = in.signal == 0 ? "battle signal off" : "battle signal not available";
        } else if (!was) {
            if (prev_signal == 0) {
                fight_ = true;
                grace_until_ = in.t + c.rise_grace_seconds;
                r.reason = "battle signal on";
            } else if (shown_polls_ >= c.enter_polls) {
                fight_ = true;
                grace_until_ = in.t;
                r.reason = "battle objects shown";
            }
        } else if (in.t >= grace_until_ && in.t - last_shown_ >= c.exit_seconds) {
            fight_ = false;
            r.reason = last_shown_ < -1e8 ? "no battle objects since the battle signal turned on"
                                          : "no battle objects shown for " + fmt_seconds(in.t - last_shown_);
        }
        r.fight = fight_;
        r.changed = fight_ != was;
        return r;
    }
    bool fight() const { return fight_; }
    // Seconds since the objects were last shown (negative: never).
    double since_shown(double t) const { return last_shown_ < -1e8 ? -1.0 : t - last_shown_; }
    void reset() { *this = Decider{}; }

private:
    static std::string fmt_seconds(double s) {
        const int tenths = static_cast<int>(s * 10.0 + 0.5);
        return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " s";
    }
    bool fight_ = false;
    int signal_ = -1;
    int shown_polls_ = 0;
    double last_shown_ = -1e9;
    double grace_until_ = -1e9;
};

}  // namespace ff7vr::engine::battle

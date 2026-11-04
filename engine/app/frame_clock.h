#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace de::app {

// The time a frame advances the camera and the world by. With vsync, frames reach the screen a whole
// number of refresh intervals apart, however unevenly the loop's clock ticks; used raw, that clock's
// noise moves the camera unevenly. So while the frame rate holds the refresh rate, the measured time
// adds up and goes out in whole intervals, over 0, the remainder carried over, and a hitch stays one
// jump. (The idea is Godot's MainTimerSync::DeltaSmoother, MIT.)
class FrameClock {
public:
    // Frames the rate is measured over: rhythms of 2, 3, 4, 5, 6 or 8 frames fit it exactly. (A rhythm
    // with over two intervals' worth of short frames in a row would run fast, past kMostAheadIntervals;
    // the loop's queue of two frames hasn't shown one.)
    static constexpr uint32_t kRateFrames = 120;

    // refresh_hz: 0 without vsync, or if it isn't known; the measured time then goes out as it is.
    double step(double measured_s, double refresh_hz) {
        if (refresh_hz != refresh_hz_) { // another display: what was measured says nothing about this one
            *this = FrameClock{};
            refresh_hz_ = refresh_hz;
        }
        if (refresh_hz <= 0.0) {
            return measured_s;
        }
        const double interval_s = 1.0 / refresh_hz;
        const bool was_on = on_;
        measure_rate(measured_s, interval_s);
        if (!on_) {
            leftover_s_ = 0.0;
            return measured_s;
        }
        if (!was_on) {
            return interval_s; // whatever this frame of the rhythm measured
        }
        leftover_s_ += measured_s;
        // To the nearest interval, so noise stays one, and at least one: the rhythm's short frames. What's
        // borrowed is bounded, so after a hitch's catch-up frames it doesn't pile up.
        const double whole = std::max(std::floor(leftover_s_ / interval_s + 0.5), 1.0);
        leftover_s_ = std::max(leftover_s_ - whole * interval_s, -interval_s * kMostAheadIntervals);
        return whole * interval_s;
    }

    bool holds_rate() const { return on_; }

private:
    // Is the screen on these intervals? It counts the refreshes that passed, a whole number, against the
    // frames. Not on them: vsync overridden, too much to draw, a variable-rate display well below its
    // maximum. It takes about one frame a refresh to turn on, and more than a few missed refreshes or a
    // hitch's catch-up frames to turn off: missed refreshes are when it's needed.
    void measure_rate(double measured_s, double interval_s) {
        if (measured_s < interval_s * kLongFrameIntervals) {
            rate_total_s_ += measured_s;
        } else {
            ++long_frames_;
        }
        if (++rate_frames_ < kRateFrames) {
            return;
        }
        const double refreshes = std::round(rate_total_s_ / interval_s);
        const double extra = refreshes - static_cast<double>(kRateFrames - long_frames_);
        if (on_) {
            on_ = long_frames_ < kMostLongFrames && extra >= -kMostAheadIntervals && extra <= kMostMissedRefreshes;
        } else {
            on_ = long_frames_ == 0 && std::abs(extra) <= kTurnOnRefreshes;
        }
        rate_total_s_ = 0.0;
        rate_frames_ = 0;
        long_frames_ = 0;
    }

    static constexpr double kLongFrameIntervals = 4.0;  // from this on, a hitch: not counted as refreshes
    static constexpr uint32_t kMostLongFrames = 15;     // per window, to stay on
    static constexpr double kMostAheadIntervals = 2.0;  // what short frames may borrow, and fewer refreshes
                                                        // than frames in a window, to stay on
    static constexpr double kMostMissedRefreshes = 6.0; // per window, to stay on: 5%
    static constexpr double kTurnOnRefreshes = 1.0;     // off from one a frame, per window, to turn on

    double refresh_hz_ = 0.0;
    double leftover_s_ = 0.0;
    double rate_total_s_ = 0.0;
    uint32_t rate_frames_ = 0;
    uint32_t long_frames_ = 0;
    bool on_ = false;
};

} // namespace de::app

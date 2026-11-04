#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace de::app {

// The time a frame advances the camera and the world by. With vsync, frames reach the screen a whole
// number of refresh intervals apart, however unevenly the loop's clock ticks; used raw, that clock's
// noise moves the camera unevenly. So while the frame rate holds the refresh rate, the measured time
// adds up and goes out in whole intervals, the remainder carried over: nothing runs slow or fast, and a
// hitch stays one jump. (The idea is Godot's MainTimerSync::DeltaSmoother, MIT.)
class FrameClock {
public:
    // refresh_hz: 0 without vsync, or if it isn't known; the measured time then goes out as it is.
    double step(double measured_s, double refresh_hz) {
        if (refresh_hz <= 0.0) {
            leftover_s_ = 0.0;
            return measured_s;
        }
        const double interval_s = 1.0 / refresh_hz;

        // Is it holding the refresh rate? Not with vsync overridden, a variable-rate display or too much
        // to draw: then the screen isn't on these intervals. A hitch alone says nothing about it.
        if (measured_s < interval_s * 4.0) {
            rate_total_s_ += measured_s;
            if (++rate_frames_ == kRateFrames) {
                const double ratio = interval_s * kRateFrames / rate_total_s_;
                holds_rate_ = ratio > 1.0 - kRateTolerance && ratio < 1.0 + kRateTolerance;
                rate_total_s_ = 0.0;
                rate_frames_ = 0;
            }
        }
        if (!holds_rate_) {
            leftover_s_ = 0.0;
            return measured_s;
        }

        leftover_s_ += measured_s;
        const double intervals = std::max(std::floor(leftover_s_ / interval_s), 1.0); // a frame takes at least one
        leftover_s_ = std::max(leftover_s_ - intervals * interval_s, -interval_s * 4.0);
        return intervals * interval_s;
    }

private:
    static constexpr uint32_t kRateFrames = 64; // frames the rate is measured over
    static constexpr double kRateTolerance = 0.05;

    double leftover_s_ = 0.0;
    double rate_total_s_ = 0.0;
    uint32_t rate_frames_ = 0;
    bool holds_rate_ = false;
};

} // namespace de::app

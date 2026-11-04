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
        if (refresh_hz != refresh_hz_) { // another display: what was measured says nothing about this one
            refresh_hz_ = refresh_hz;
            holds_rate_ = false;
            rate_total_s_ = 0.0;
            rate_frames_ = 0;
            long_frames_ = 0;
            leftover_s_ = 0.0;
        }
        if (refresh_hz <= 0.0) {
            return measured_s;
        }
        const double interval_s = 1.0 / refresh_hz;
        measure_rate(measured_s, interval_s);
        if (!holds_rate_) {
            leftover_s_ = 0.0;
            return measured_s;
        }

        leftover_s_ += measured_s;
        const double whole = std::floor(leftover_s_ / interval_s);
        // A frame takes at least one interval, even with less measured: the loop's rhythm. So far ahead
        // of the time measured, though, it takes none, so the clock can't run fast.
        const double intervals = whole >= 1.0 ? whole : (leftover_s_ > -interval_s * 2.0 ? 1.0 : 0.0);
        leftover_s_ -= intervals * interval_s;
        return intervals * interval_s;
    }

    bool holds_rate() const { return holds_rate_; }

private:
    // Is it on the refresh rate? Not with vsync overridden, too much to draw or a variable-rate display
    // running below it: then the screen isn't on these intervals. A hitch or two say nothing about it.
    void measure_rate(double measured_s, double interval_s) {
        if (measured_s < interval_s * 4.0) {
            rate_total_s_ += measured_s;
        } else {
            ++long_frames_;
        }
        if (++rate_frames_ == kRateFrames) {
            const double even_frames = kRateFrames - long_frames_;
            const double ratio = rate_total_s_ > 0.0 ? interval_s * even_frames / rate_total_s_ : 0.0;
            holds_rate_ = long_frames_ < kRateFrames / 8 && std::abs(ratio - 1.0) < kRateTolerance;
            rate_total_s_ = 0.0;
            rate_frames_ = 0;
            long_frames_ = 0;
        }
    }

    static constexpr uint32_t kRateFrames = 128; // frames the rate is measured over
    static constexpr double kRateTolerance = 0.01;

    double refresh_hz_ = 0.0;
    double leftover_s_ = 0.0;
    double rate_total_s_ = 0.0;
    uint32_t rate_frames_ = 0;
    uint32_t long_frames_ = 0;
    bool holds_rate_ = false;
};

} // namespace de::app

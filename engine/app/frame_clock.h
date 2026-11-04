#pragma once

#include <cmath>
#include <cstdint>

namespace de::app {

// The time a frame advances the camera and the world by. With vsync, frames reach the screen a whole
// number of refresh intervals apart, however unevenly the loop's clock ticks; used raw, that clock's
// noise moves the camera unevenly. So while the frame rate holds the refresh rate, the measured time
// adds up and goes out in whole intervals, the remainder carried over, and a hitch stays one jump. (The
// idea is Godot's MainTimerSync::DeltaSmoother, MIT.) What's handed out adds up to what's measured, but
// for what's still carried when it turns off: three intervals at most, and rarely.
class FrameClock {
public:
    // refresh_hz: 0 without vsync, or if it isn't known; the measured time then goes out as it is.
    double step(double measured_s, double refresh_hz) {
        if (refresh_hz != refresh_hz_) { // another display: what was measured says nothing about this one
            refresh_hz_ = refresh_hz;
            on_ = false;
            rate_total_s_ = 0.0;
            rate_frames_ = 0;
            long_frames_ = 0;
            leftover_s_ = 0.0;
        }
        if (refresh_hz <= 0.0) {
            return measured_s;
        }
        measure_rate(measured_s, 1.0 / refresh_hz);
        if (!on_) {
            leftover_s_ = 0.0;
            return measured_s;
        }

        leftover_s_ += measured_s;
        const double whole = std::floor(leftover_s_ / interval_s_ + 0.5); // to the nearest: noise stays one
        if (whole >= 1.0) {
            leftover_s_ -= whole * interval_s_;
            return whole * interval_s_;
        }
        // Less than an interval measured: the loop's rhythm, it still takes one. So far ahead of the time
        // measured, just what it measured, so the clock can't run fast, and not 0: code may divide by this.
        const double short_step = leftover_s_ > -interval_s_ * 2.0 ? interval_s_ : measured_s;
        leftover_s_ -= short_step;
        return short_step;
    }

    bool holds_rate() const { return on_; }

private:
    // Is the screen on these intervals? Not with vsync overridden, too much to draw or a variable-rate
    // display below its maximum. It takes a rate close to the display's to turn on, and more than a few
    // missed refreshes or hitches to turn off: that's when it's needed. It steps by the interval it
    // measures, not the one reported: a display a little off would get double steps.
    void measure_rate(double measured_s, double reported_interval_s) {
        if (measured_s < reported_interval_s * 4.0) {
            rate_total_s_ += measured_s;
        } else {
            ++long_frames_;
        }
        if (++rate_frames_ < kRateFrames) {
            return;
        }
        const uint32_t even_frames = kRateFrames - long_frames_;
        const double mean_s = even_frames > 0 ? rate_total_s_ / even_frames : 0.0;
        const double ratio = mean_s > 0.0 ? reported_interval_s / mean_s : 0.0;
        if (!on_) {
            on_ = long_frames_ == 0 && std::abs(ratio - 1.0) < kOnBand;
            if (on_) {
                interval_s_ = mean_s;
            }
        } else {
            on_ = long_frames_ < kRateFrames / 8 && ratio > kStayBehind && ratio < kStayAhead;
            if (std::abs(ratio - 1.0) < kOnBand) {
                interval_s_ += (mean_s - interval_s_) / 8.0;
            }
        }
        rate_total_s_ = 0.0;
        rate_frames_ = 0;
        long_frames_ = 0;
    }

    // Frames the rate is measured over: rhythms of 2, 3, 4, 5, 6 or 8 frames fit it exactly.
    static constexpr uint32_t kRateFrames = 120;
    static constexpr double kOnBand = 0.01;
    static constexpr double kStayBehind = 0.90; // the display's rate over the measured: 10% of frames missed
    static constexpr double kStayAhead = 1.03;

    double refresh_hz_ = 0.0;
    double interval_s_ = 0.0;
    double leftover_s_ = 0.0;
    double rate_total_s_ = 0.0;
    uint32_t rate_frames_ = 0;
    uint32_t long_frames_ = 0;
    bool on_ = false;
};

} // namespace de::app

#pragma once

#include <algorithm>
#include <cstddef>

namespace de::app {

// The time a frame advances the camera and the world by: the mean of the last frames' measured times.
// What a frame measures is when the loop came around, not when the frame is seen, and it's noisy: a
// display shows frames evenly while the loop's clock doesn't tick evenly. Used raw, every bit of that
// noise moves the camera unevenly. A mean is unbiased: the time it hands out adds up to the time
// measured, so nothing runs slow or fast, and a real hitch is caught up over a few frames.
class FrameClock {
public:
    double step(double measured_s) {
        window_[next_] = measured_s;
        next_ = (next_ + 1) % kWindow;
        count_ = std::min(count_ + 1, kWindow);
        double sum = 0.0;
        for (const double value : window_) {
            sum += value;
        }
        return sum / static_cast<double>(count_);
    }

private:
    // Divisible by 2, 3 and 4: a rhythm that repeats every few frames cancels exactly.
    static constexpr size_t kWindow = 12;

    double window_[kWindow] = {};
    size_t next_ = 0;
    size_t count_ = 0;
};

} // namespace de::app

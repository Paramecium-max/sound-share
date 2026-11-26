#pragma once

#include <array>
#include <chrono>
#include <optional>

namespace miplay::discovery {

class ProbeBurst {
public:
    using Time = std::chrono::steady_clock::time_point;

    bool active() const { return step_ < offsets_.size(); }

    bool begin(Time now) {
        if (active()) return false;
        origin_ = now;
        step_ = 0;
        return true;
    }

    Time deadline() const { return origin_ + offsets_[step_]; }

    std::optional<size_t> take_due(Time now) {
        if (!active() || now < deadline()) return std::nullopt;
        return step_++;
    }

private:
    static constexpr std::array offsets_{
        std::chrono::milliseconds(0), std::chrono::milliseconds(220), std::chrono::milliseconds(650),
    };
    Time origin_{};
    size_t step_ = offsets_.size();
};

} // namespace miplay::discovery

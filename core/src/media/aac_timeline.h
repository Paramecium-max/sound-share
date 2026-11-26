#pragma once

#include <cstdint>
#include <deque>
#include <optional>

namespace miplay::media {

enum class AacInputResult { Accepted, Backpressure, Failed };

// Media Foundation's AAC-LC transform returns AUs in input order. One timestamp
// belongs to each accepted 1024-sample PCM block, including across a drain/retry.
class AacTimeline {
public:
    template<class Submit, class Drain>
    bool submit(uint64_t pts, Submit send_input, Drain drain) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (accepted_.size() >= 64) return false;
            switch (send_input()) {
            case AacInputResult::Accepted:
                accepted_.push_back(pts);
                return drain();
            case AacInputResult::Backpressure:
                if (!drain()) return false;
                break;
            case AacInputResult::Failed:
                return false;
            }
        }
        return false;
    }

    std::optional<uint64_t> take_output() {
        if (accepted_.empty()) return std::nullopt;
        auto pts = accepted_.front();
        accepted_.pop_front();
        return pts;
    }

    void clear() { accepted_.clear(); }

private:
    std::deque<uint64_t> accepted_;
};

} // namespace miplay::media

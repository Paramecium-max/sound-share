#pragma once

#include "platform/net_platform.h"

#include <chrono>

namespace miplay::net {

template<class Clock, class Send, class Wait>
SendAllStatus write_until(const uint8_t* data, size_t length, std::chrono::steady_clock::time_point deadline,
                          const std::atomic<bool>* cancel, Clock now, Send send, Wait wait) {
    size_t sent = 0;
    bool would_block = false;
    while (sent < length) {
        const auto time = now();
        if (cancel && cancel->load()) return sent ? SendAllStatus::Partial : SendAllStatus::Cancelled;
        if (time >= deadline) {
            if (sent) return SendAllStatus::Partial;
            return would_block ? SendAllStatus::TimeoutClean : SendAllStatus::Failed;
        }
        const auto attempt = send(data + sent, length - sent);
        switch (attempt.state) {
        case WriteAttempt::State::Progress:
            if (attempt.bytes == 0 || attempt.bytes > length - sent) {
                return sent ? SendAllStatus::Partial : SendAllStatus::Failed;
            }
            sent += attempt.bytes;
            break;
        case WriteAttempt::State::Interrupted:
            break;
        case WriteAttempt::State::WouldBlock:
            would_block = true;
            wait(deadline);
            break;
        case WriteAttempt::State::Failed:
            return sent ? SendAllStatus::Partial : SendAllStatus::Failed;
        }
    }
    return SendAllStatus::Ok;
}

} // namespace miplay::net

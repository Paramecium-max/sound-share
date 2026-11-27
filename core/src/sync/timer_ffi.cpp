#include "sync/timer_service.h"
#include "util/json.h"
#include <memory>

namespace {
struct Timer {
    miplay::sync::TimerService service;
    std::string snapshot;
};
}

extern "C" void* soundflow_timer_start() noexcept {
    try {
        auto timer = std::make_unique<Timer>();
        if (!timer->service.start()) return nullptr;
        return timer.release();
    } catch (...) {
        return nullptr;
    }
}

extern "C" void soundflow_timer_stop(void* pointer) noexcept {
    delete static_cast<Timer*>(pointer);
}

extern "C" uint16_t soundflow_timer_port(void* pointer) noexcept {
    auto* timer = static_cast<Timer*>(pointer);
    return timer ? timer->service.port() : 0;
}

extern "C" const char* soundflow_timer_snapshot(void* pointer) noexcept {
    auto* timer = static_cast<Timer*>(pointer);
    if (!timer) return nullptr;
    try {
        miplay::json::Value::Array snapshot;
        for (const auto& [ip, client] : timer->service.snapshot_telemetry()) {
            snapshot.emplace_back(miplay::json::Value::Object{
                {"ip", ip}, {"port", client.client_port}, {"seq", client.last_seq},
                {"seen", client.last_seen_us}, {"residual", client.residual_error_us}, {"count", client.packet_count},
            });
        }
        timer->snapshot = miplay::json::dump(snapshot);
        return timer->snapshot.c_str();
    } catch (...) {
        return nullptr;
    }
}

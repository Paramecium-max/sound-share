#include "sidecar/sender_rpc.h"

#include <chrono>
#include <deque>
#include <mutex>
#include <string_view>

#ifdef _WIN32
#define SOUNDFLOW_EXPORT __declspec(dllexport)
#else
#define SOUNDFLOW_EXPORT __attribute__((visibility("default")))
#endif

namespace {
using Clock = std::chrono::steady_clock;

struct Sender {
    std::mutex mutex;
    std::deque<miplay::json::Value> events;
    std::string response;
    Clock::time_point next_tick = Clock::now();
    miplay::MultiRoomEngine engine;

    Sender() {
        engine.set_reject_self_ip(true);
        engine.set_event_callback([this](const std::string& raw) {
            auto event = miplay::json::parse(raw);
            const auto* fields = std::get_if<miplay::json::Value::Object>(&event.data);
            if (!fields || !fields->contains("type")) return;
            const auto* type = std::get_if<std::string>(&fields->at("type").data);
            // Poll returns a fresh state snapshot. Keep only transient events.
            if (!type || *type == "state") return;
            std::lock_guard lock(mutex);
            if (*type == "spectrum" && !events.empty()) {
                auto* last = std::get_if<miplay::json::Value::Object>(&events.back().data);
                auto* last_type = last ? std::get_if<std::string>(&last->at("type").data) : nullptr;
                if (last_type && *last_type == "spectrum") events.pop_back();
            }
            if (events.size() >= 64) events.pop_front();
            events.push_back(std::move(event));
        });
    }

    ~Sender() {
        engine.set_event_callback(nullptr);
        engine.stop_multiroom();
        engine.stop_discovery();
    }

    const char* request(std::string_view request) {
        if (!request.empty()) {
            response = miplay::sidecar::handle_request(engine, std::string(request), true);
        } else {
            auto now = Clock::now();
            if (now >= next_tick) {
                engine.tick_media_sync();
                next_tick = now + std::chrono::seconds(1);
            }
            miplay::json::Value::Array pending;
            {
                std::lock_guard lock(mutex);
                pending.assign(std::make_move_iterator(events.begin()), std::make_move_iterator(events.end()));
                events.clear();
            }
            response = miplay::json::dump(miplay::json::Value::Object{
                {"data", engine.snapshot_state_value()}, {"events", std::move(pending)},
            });
        }
        return response.c_str();
    }
};
} // namespace

// A context has one caller. The returned UTF-8 response is owned by the context
// until the next request or destruction. Passing null polls the event queue.
extern "C" SOUNDFLOW_EXPORT void* soundflow_sender_create() noexcept {
    try { return new Sender; } catch (...) { return nullptr; }
}

extern "C" SOUNDFLOW_EXPORT const char* soundflow_sender_request(void* context, const char* json) noexcept {
    if (!context) return nullptr;
    try { return static_cast<Sender*>(context)->request(json ? json : ""); }
    catch (...) { return nullptr; }
}

extern "C" SOUNDFLOW_EXPORT void soundflow_sender_push_pcm(
    void* context, const int16_t* pcm, size_t frames) noexcept {
    if (!context || !pcm || frames == 0) return;
    try { static_cast<Sender*>(context)->engine.push_external_pcm(pcm, frames); }
    catch (...) {}
}

extern "C" SOUNDFLOW_EXPORT void soundflow_sender_destroy(void* context) noexcept {
    delete static_cast<Sender*>(context);
}

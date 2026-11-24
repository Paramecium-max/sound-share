#include "miplay_core/miplay_c_api.h"
#include "engine/multiroom_engine.h"

#include <memory>
#include <string>

struct MiPlayEngineHandle {
    std::unique_ptr<miplay::MultiRoomEngine> engine;
    MiPlayEventCallback cb = nullptr;
    void* user_data = nullptr;
};

extern "C" {

MiPlayEngineHandle* miplay_engine_create(const char* bind_ip) {
    auto* h = new MiPlayEngineHandle();
    h->engine = std::make_unique<miplay::MultiRoomEngine>(bind_ip ? bind_ip : "");
    return h;
}

void miplay_engine_destroy(MiPlayEngineHandle* handle) {
    delete handle;
}

void miplay_engine_set_event_callback(
    MiPlayEngineHandle* handle, MiPlayEventCallback cb, void* user_data) {
    if (!handle) return;
    handle->cb = cb;
    handle->user_data = user_data;
    if (cb) {
        handle->engine->set_event_callback([handle](const std::string& json) {
            if (handle->cb) handle->cb(json.c_str(), handle->user_data);
        });
    } else {
        handle->engine->set_event_callback(nullptr);
    }
}

int miplay_engine_start_discovery(MiPlayEngineHandle* handle) {
    return (handle && handle->engine->start_discovery()) ? 0 : -1;
}

int miplay_engine_stop_discovery(MiPlayEngineHandle* handle) {
    if (!handle) return -1;
    handle->engine->stop_discovery();
    return 0;
}

int miplay_engine_add_manual_device(
    MiPlayEngineHandle* handle, const char* name, const char* ip, uint16_t cmd_port) {
    if (!handle || !ip) return -1;
    handle->engine->add_manual_device(name ? name : "", ip, cmd_port);
    return 0;
}

int miplay_engine_start_multiroom(
    MiPlayEngineHandle* handle, MiPlayAudioCodec codec, uint32_t sample_rate, uint32_t base_play_delay_ms) {
    if (!handle) return -1;
    bool prefer_lpcm = (codec == MIPLAY_CODEC_LPCM);
    return handle->engine->start_multiroom(prefer_lpcm, sample_rate, base_play_delay_ms) ? 0 : -1;
}

int miplay_engine_stop_multiroom(MiPlayEngineHandle* handle) {
    if (!handle) return -1;
    handle->engine->stop_multiroom();
    return 0;
}

int miplay_engine_add_device_to_group(MiPlayEngineHandle* handle, const char* device_id) {
    return (handle && device_id && handle->engine->add_device_to_group(device_id)) ? 0 : -1;
}

int miplay_engine_remove_device_from_group(MiPlayEngineHandle* handle, const char* device_id) {
    return (handle && device_id && handle->engine->remove_device_from_group(device_id)) ? 0 : -1;
}

int miplay_engine_set_device_delay_bias(
    MiPlayEngineHandle* handle, const char* device_id, int32_t delay_bias_ms) {
    return (handle && device_id && handle->engine->set_device_delay_bias(device_id, delay_bias_ms)) ? 0 : -1;
}

int miplay_engine_set_device_volume(
    MiPlayEngineHandle* handle, const char* device_id, int32_t volume_0_100) {
    return (handle && device_id && handle->engine->set_device_volume(device_id, volume_0_100)) ? 0 : -1;
}

int miplay_engine_set_source_mode(
    MiPlayEngineHandle* handle, MiPlaySourceMode mode, const char* param) {
    if (!handle) return -1;
    return handle->engine->set_source_mode(
        static_cast<miplay::media::AudioSourceMode>(mode), param ? param : "") ? 0 : -1;
}

int miplay_engine_push_pcm(
    MiPlayEngineHandle* handle, const int16_t* interleaved_stereo_pcm, size_t frame_count) {
    if (!handle || !interleaved_stereo_pcm) return -1;
    handle->engine->push_external_pcm(interleaved_stereo_pcm, frame_count);
    return 0;
}

} // extern "C"

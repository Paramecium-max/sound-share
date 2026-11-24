#pragma once

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
  #define MIPLAY_API __declspec(dllexport)
#else
  #define MIPLAY_API __attribute__((visibility("default")))
#endif

typedef struct MiPlayEngineHandle MiPlayEngineHandle;

typedef enum MiPlayAudioCodec {
    MIPLAY_CODEC_AAC_LC = 0,
    MIPLAY_CODEC_LPCM   = 1,
    MIPLAY_CODEC_FLAC   = 2
} MiPlayAudioCodec;

typedef enum MiPlaySourceMode {
    MIPLAY_SOURCE_TEST_TONE      = 0,
    MIPLAY_SOURCE_WAV_FILE       = 1,
    MIPLAY_SOURCE_SYSTEM_LOOPBACK = 2,
    MIPLAY_SOURCE_EXTERNAL_PCM   = 3
} MiPlaySourceMode;

typedef struct MiPlayDeviceInfo {
    char id[64];
    char name[128];
    char ip[64];
    char mac[32];
    uint16_t cmd_port;
    int32_t device_type;
    int32_t user_delay_bias_ms;
    int32_t volume;
    int32_t connected;
    int32_t streaming;
    double rtt_ms;
    double offset_us;
} MiPlayDeviceInfo;

typedef void (*MiPlayEventCallback)(const char* event_json, void* user_data);

MIPLAY_API MiPlayEngineHandle* miplay_engine_create(const char* bind_ip);
MIPLAY_API void miplay_engine_destroy(MiPlayEngineHandle* handle);

MIPLAY_API void miplay_engine_set_event_callback(
    MiPlayEngineHandle* handle,
    MiPlayEventCallback cb,
    void* user_data);

MIPLAY_API int miplay_engine_start_discovery(MiPlayEngineHandle* handle);
MIPLAY_API int miplay_engine_stop_discovery(MiPlayEngineHandle* handle);

MIPLAY_API int miplay_engine_add_manual_device(
    MiPlayEngineHandle* handle,
    const char* name,
    const char* ip,
    uint16_t cmd_port);

MIPLAY_API int miplay_engine_start_multiroom(
    MiPlayEngineHandle* handle,
    MiPlayAudioCodec codec,
    uint32_t sample_rate,
    uint32_t base_play_delay_ms);

MIPLAY_API int miplay_engine_stop_multiroom(MiPlayEngineHandle* handle);

MIPLAY_API int miplay_engine_add_device_to_group(
    MiPlayEngineHandle* handle,
    const char* device_id);

MIPLAY_API int miplay_engine_remove_device_from_group(
    MiPlayEngineHandle* handle,
    const char* device_id);

MIPLAY_API int miplay_engine_set_device_delay_bias(
    MiPlayEngineHandle* handle,
    const char* device_id,
    int32_t delay_bias_ms);

MIPLAY_API int miplay_engine_set_device_volume(
    MiPlayEngineHandle* handle,
    const char* device_id,
    int32_t volume_0_100);

MIPLAY_API int miplay_engine_set_source_mode(
    MiPlayEngineHandle* handle,
    MiPlaySourceMode mode,
    const char* param);

MIPLAY_API int miplay_engine_push_pcm(
    MiPlayEngineHandle* handle,
    const int16_t* interleaved_stereo_pcm,
    size_t frame_count);

#ifdef __cplusplus
}
#endif

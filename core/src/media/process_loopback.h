#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <audioclient.h>
#endif

namespace miplay::media {

struct AudioApp {
    uint32_t pid;
    std::string image;
    std::string label;
};

std::vector<AudioApp> list_audio_apps();
std::optional<uint32_t> process_loopback_pid(std::string_view param);

#ifdef _WIN32
bool is_legacy_process_loopback_windows();
HRESULT activate_process_loopback(uint32_t pid, IAudioClient** client);
#endif

} // namespace miplay::media

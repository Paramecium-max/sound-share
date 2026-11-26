#pragma once

#include "cmd/cmd_codec.h"

#include <cstdint>
#include <string>
#include <vector>

namespace miplay::media {

struct LrcLine {
    uint64_t time_ms = 0;
    std::string text;
};

// 标准 LRC 歌词解析器：支持 [mm:ss.xx] / [mm:ss.xxx] / [mm:ss]、一行多时间标签、[offset:ms] 及 UTF-8 BOM
class LrcDocument {
public:
    static LrcDocument parse(const std::string& lrc_text);
    static std::string load_text_file(const std::string& path);
    static std::string load_sidecar_lrc_for_wav(const std::string& wav_path);

    bool empty() const { return lines_.empty(); }
    size_t size() const { return lines_.size(); }
    const std::string& raw_text() const { return raw_text_; }
    const std::vector<LrcLine>& lines() const { return lines_; }

    std::string line_at(uint64_t position_ms) const;

private:
    std::string raw_text_;
    std::vector<LrcLine> lines_;
};

// 跨平台系统媒体控制桥接：
// - Windows: Win32 SendInput 硬件媒体键 (VK_MEDIA_PLAY_PAUSE / NEXT_TRACK / PREV_TRACK)
// - Linux: 非阻塞尝试 MPRIS playerctl (pause / play / next / previous / position)
class SystemMediaController {
public:
    static bool dispatch_action(cmd::RemoteControlAction action, uint64_t position_ms = 0);
    static const char* action_name(cmd::RemoteControlAction action);
};

} // namespace miplay::media

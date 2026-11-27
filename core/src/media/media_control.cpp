#include "media/media_control.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__) && !defined(__ANDROID__)
#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>
extern char** environ;
#endif

namespace miplay::media {

namespace {

std::string trim_copy(std::string_view sv) {
    size_t start = 0;
    while (start < sv.size() && std::isspace(static_cast<unsigned char>(sv[start]))) ++start;
    size_t end = sv.size();
    while (end > start && std::isspace(static_cast<unsigned char>(sv[end - 1]))) --end;
    return std::string(sv.substr(start, end - start));
}

bool parse_timestamp_tag(std::string_view tag, uint64_t& out_ms) {
    // 格式: mm:ss 或 mm:ss.xx 或 mm:ss.xxx（也兼容 mm:ss:xx）
    const size_t colon = tag.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= tag.size()) return false;
    uint64_t mins = 0;
    for (size_t i = 0; i < colon; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(tag[i]))) return false;
        mins = mins * 10 + static_cast<uint64_t>(tag[i] - '0');
        if (mins > 9999) return false;
    }
    size_t pos = colon + 1;
    uint64_t secs = 0;
    size_t sec_digits = 0;
    while (pos < tag.size() && std::isdigit(static_cast<unsigned char>(tag[pos]))) {
        secs = secs * 10 + static_cast<uint64_t>(tag[pos] - '0');
        ++pos;
        ++sec_digits;
        if (sec_digits > 2) return false;
    }
    if (sec_digits == 0 || secs >= 60) return false;

    uint64_t frac_ms = 0;
    if (pos < tag.size()) {
        if (tag[pos] != '.' && tag[pos] != ':') return false;
        ++pos;
        size_t frac_digits = 0;
        uint64_t frac = 0;
        while (pos < tag.size() && std::isdigit(static_cast<unsigned char>(tag[pos]))) {
            if (frac_digits < 3) {
                frac = frac * 10 + static_cast<uint64_t>(tag[pos] - '0');
            }
            ++frac_digits;
            ++pos;
        }
        if (pos != tag.size() || frac_digits == 0) return false;
        if (frac_digits == 1) frac_ms = frac * 100;
        else if (frac_digits == 2) frac_ms = frac * 10;
        else frac_ms = frac;
    }

    out_ms = (mins * 60ULL + secs) * 1000ULL + frac_ms;
    return true;
}

} // namespace

LrcDocument LrcDocument::parse(const std::string& lrc_text) {
    LrcDocument doc;
    if (lrc_text.empty()) return doc;

    std::string cleaned = lrc_text;
    // 剥离 UTF-8 BOM
    if (cleaned.size() >= 3 &&
        static_cast<uint8_t>(cleaned[0]) == 0xEF &&
        static_cast<uint8_t>(cleaned[1]) == 0xBB &&
        static_cast<uint8_t>(cleaned[2]) == 0xBF) {
        cleaned.erase(0, 3);
    }
    doc.raw_text_ = cleaned;

    int64_t offset_ms = 0;
    std::vector<std::pair<int64_t, std::string>> entries;
    std::istringstream iss(cleaned);
    std::string raw_line;
    while (std::getline(iss, raw_line)) {
        if (!raw_line.empty() && raw_line.back() == '\r') raw_line.pop_back();
        if (raw_line.empty()) continue;

        std::vector<uint64_t> line_times;
        size_t pos = 0;
        while (pos < raw_line.size() && raw_line[pos] == '[') {
            const size_t close = raw_line.find(']', pos + 1);
            if (close == std::string::npos) break;
            const std::string_view tag(raw_line.data() + pos + 1, close - pos - 1);
            uint64_t ts = 0;
            if (parse_timestamp_tag(tag, ts)) {
                line_times.push_back(ts);
            } else if (tag.size() > 7 &&
                       (tag.rfind("offset:", 0) == 0 || tag.rfind("OFFSET:", 0) == 0)) {
                const std::string val = trim_copy(tag.substr(7));
                try {
                    offset_ms = std::stoll(val);
                } catch (...) {}
            }
            pos = close + 1;
        }
        if (line_times.empty()) continue;
        const std::string lyric = trim_copy(std::string_view(raw_line).substr(pos));
        for (uint64_t t : line_times) {
            entries.emplace_back(static_cast<int64_t>(t), lyric);
        }
    }

    doc.lines_.reserve(entries.size());
    for (auto& [t, text] : entries) {
        // 标准 LRC offset 正值表示歌词提前显示，故时间戳减 offset
        int64_t adjusted = t - offset_ms;
        if (adjusted < 0) adjusted = 0;
        doc.lines_.push_back(LrcLine{static_cast<uint64_t>(adjusted), std::move(text)});
    }
    std::stable_sort(doc.lines_.begin(), doc.lines_.end(),
                     [](const LrcLine& a, const LrcLine& b) { return a.time_ms < b.time_ms; });
    return doc;
}

std::string LrcDocument::load_text_file(const std::string& path) {
    if (path.empty() || path.find('\0') != std::string::npos) return {};
    try {
        const std::filesystem::path native_path(
            std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size()));
        std::ifstream ifs(native_path, std::ios::binary | std::ios::ate);
        if (!ifs) return {};
        const auto sz = ifs.tellg();
        if (sz <= 0 || sz > 2 * 1024 * 1024) return {};
        std::string out(static_cast<size_t>(sz), '\0');
        ifs.seekg(0, std::ios::beg);
        if (!ifs.read(out.data(), sz)) return {};
        return out;
    } catch (...) {
        return {};
    }
}

std::string LrcDocument::load_sidecar_lrc_for_wav(const std::string& wav_path) {
    if (wav_path.empty() || wav_path.find('\0') != std::string::npos) return {};
    try {
        std::filesystem::path p(
            std::u8string(reinterpret_cast<const char8_t*>(wav_path.data()), wav_path.size()));
        p.replace_extension(".lrc");
        std::error_code ec;
        if (!std::filesystem::is_regular_file(p, ec)) return {};
        const auto u8 = p.u8string();
        std::string lrc_path(reinterpret_cast<const char*>(u8.data()), u8.size());
        return load_text_file(lrc_path);
    } catch (...) {
        return {};
    }
}

std::string LrcDocument::line_at(uint64_t position_ms) const {
    if (lines_.empty()) return {};
    auto it = std::upper_bound(
        lines_.begin(), lines_.end(), position_ms,
        [](uint64_t ms, const LrcLine& line) { return ms < line.time_ms; });
    if (it == lines_.begin()) return {};
    --it;
    return it->text;
}

const char* SystemMediaController::action_name(cmd::RemoteControlAction action) {
    switch (action) {
        case cmd::RemoteControlAction::Pause: return "pause";
        case cmd::RemoteControlAction::Resume: return "resume";
        case cmd::RemoteControlAction::Next: return "next";
        case cmd::RemoteControlAction::Previous: return "previous";
        case cmd::RemoteControlAction::Seek: return "seek";
        case cmd::RemoteControlAction::SetVolume: return "volume";
        default: return "unknown";
    }
}

bool SystemMediaController::dispatch_action(cmd::RemoteControlAction action, uint64_t position_ms) {
#ifdef _WIN32
    (void)position_ms;
    WORD vk = 0;
    switch (action) {
        case cmd::RemoteControlAction::Pause:
        case cmd::RemoteControlAction::Resume:
            vk = VK_MEDIA_PLAY_PAUSE;
            break;
        case cmd::RemoteControlAction::Next:
            vk = VK_MEDIA_NEXT_TRACK;
            break;
        case cmd::RemoteControlAction::Previous:
            vk = VK_MEDIA_PREV_TRACK;
            break;
        default:
            return false;
    }
    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = vk;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = vk;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    return ::SendInput(2, inputs, sizeof(INPUT)) == 2;
#elif defined(__linux__) && !defined(__ANDROID__)
    std::vector<std::string> argv = {"playerctl"};
    switch (action) {
        case cmd::RemoteControlAction::Pause:
            argv.push_back("pause");
            break;
        case cmd::RemoteControlAction::Resume:
            argv.push_back("play");
            break;
        case cmd::RemoteControlAction::Next:
            argv.push_back("next");
            break;
        case cmd::RemoteControlAction::Previous:
            argv.push_back("previous");
            break;
        case cmd::RemoteControlAction::Seek: {
            std::ostringstream oss;
            oss << (static_cast<double>(position_ms) / 1000.0);
            argv.push_back("position");
            argv.push_back(oss.str());
            break;
        }
        default:
            return false;
    }
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (auto& s : argv) cargv.push_back(s.data());
    cargv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    if (::posix_spawn_file_actions_init(&actions) != 0) return false;
    ::posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    ::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    ::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid = -1;
    const int rc = ::posix_spawnp(&pid, cargv[0], &actions, nullptr, cargv.data(), environ);
    ::posix_spawn_file_actions_destroy(&actions);
    return rc == 0;
#else
    (void)action;
    (void)position_ms;
    return false;
#endif
}

} // namespace miplay::media

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace miplay::media {

// 跨平台实时 ADTS AAC-LC 音频编码器：
// - Windows: 动态调用系统原生 mfplat.dll (Media Foundation AAC MFT)，零第三方 DLL 依赖
// - Linux/macOS: 尝试可用编码管线，不可用时 is_available() 返回 false 供引擎优雅回退 LPCM
class AacEncoder {
public:
    AacEncoder();
    ~AacEncoder();

    bool init(uint32_t sample_rate = 48000, uint32_t channels = 2, uint32_t bitrate = 256000);
    void shutdown();
    bool is_available() const;

    // 输入 interleaved stereo int16 PCM (建议每次 1024 采样)，回调输出带 7 字节 ADTS 头的完整 AAC 帧
    using AdtsCallback = std::function<void(const uint8_t* adts_data, size_t len)>;
    void encode_pcm(const int16_t* stereo_pcm, size_t sample_frames, const AdtsCallback& cb);

    static std::vector<uint8_t> wrap_adts(const uint8_t* raw_aac, size_t len, uint32_t sample_rate, uint32_t channels);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace miplay::media

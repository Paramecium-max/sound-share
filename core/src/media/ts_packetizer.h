#pragma once

#include <cstdint>
#include <vector>

namespace miplay::media {

enum class TsAudioStreamType : uint8_t {
    AdtsAac = 0x0F, // 实机实证 PMT stream_type=0x0F (ISO/IEC 13818-7 ADTS AAC)
    WfdLpcm = 0x83, // Wi-Fi Display LPCM (Big-Endian 16-bit 48kHz Stereo)
};

// 严格对齐红米 K90 实机抓包格式的 MPEG-TS 打包器
// PAT(0x0000) / PMT(0x0100) / PCR(0x1000) / Audio PES(0x1100)
class TsPacketizer {
public:
    explicit TsPacketizer(TsAudioStreamType stream_type = TsAudioStreamType::AdtsAac);

    void set_stream_type(TsAudioStreamType stream_type);
    void reset();

    // 打包一帧音频为若干个 188 字节 TS 包 (定期自动插入 PAT + PMT + PCR)
    std::vector<uint8_t> packetize_audio_frame(
        const uint8_t* audio_payload,
        size_t payload_len,
        uint64_t pts_90khz,
        bool force_pat_pmt = false);

    // WFD LPCM 4 字节子头 (Big-Endian 48kHz / 44.1kHz 16-bit Stereo)
    static std::vector<uint8_t> wrap_wfd_lpcm(const int16_t* interleaved_stereo,
                                              size_t frame_samples,
                                              uint32_t sample_rate = 48000);

private:
    void append_pat(std::vector<uint8_t>& out);
    void append_pmt(std::vector<uint8_t>& out);
    void append_pcr(std::vector<uint8_t>& out, uint64_t pcr_90khz);
    static uint32_t mpeg_crc32(const uint8_t* data, size_t len);

    TsAudioStreamType stream_type_;
    uint8_t cc_pat_ = 0;
    uint8_t cc_pmt_ = 0;
    uint8_t cc_pcr_ = 0;
    uint8_t cc_audio_ = 0;
    uint64_t frame_counter_ = 0;
};

} // namespace miplay::media

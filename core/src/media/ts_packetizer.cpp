#include "media/ts_packetizer.h"

#include <algorithm>
#include <cstring>

namespace miplay::media {

namespace {
constexpr uint16_t kPidPat   = 0x0000;
constexpr uint16_t kPidPmt   = 0x0100;
constexpr uint16_t kPidPcr   = 0x1000;
constexpr uint16_t kPidAudio = 0x1100;
constexpr size_t   kTsSize   = 188;
} // namespace

TsPacketizer::TsPacketizer(TsAudioStreamType stream_type)
    : stream_type_(stream_type) {}

void TsPacketizer::set_stream_type(TsAudioStreamType stream_type) {
    stream_type_ = stream_type;
}

void TsPacketizer::reset() {
    cc_pat_ = cc_pmt_ = cc_pcr_ = cc_audio_ = 0;
    frame_counter_ = 0;
}

uint32_t TsPacketizer::mpeg_crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint32_t>(data[i]) << 24;
        for (int b = 0; b < 8; ++b) {
            if (crc & 0x80000000U) {
                crc = (crc << 1) ^ 0x04C11DB7U;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

void TsPacketizer::append_pat(std::vector<uint8_t>& out) {
    uint8_t pkt[kTsSize];
    std::memset(pkt, 0xFF, kTsSize);
    pkt[0] = 0x47;
    pkt[1] = 0x40 | ((kPidPat >> 8) & 0x1F);
    pkt[2] = kPidPat & 0xFF;
    pkt[3] = 0x10 | (cc_pat_++ & 0x0F);
    pkt[4] = 0x00; // pointer_field

    uint8_t* sec = pkt + 5;
    sec[0] = 0x00; // table_id = PAT
    sec[1] = 0xB0;
    sec[2] = 0x0D; // section_length = 13
    sec[3] = 0x00; // transport_stream_id
    sec[4] = 0x00;
    sec[5] = 0xC3; // version=1, current_next=1 (与 K90 抓包一致)
    sec[6] = 0x00; // section_number
    sec[7] = 0x00; // last_section_number
    sec[8] = 0x00; // program_number = 1
    sec[9] = 0x01;
    sec[10] = 0xE0 | ((kPidPmt >> 8) & 0x1F);
    sec[11] = kPidPmt & 0xFF;

    uint32_t crc = mpeg_crc32(sec, 12);
    sec[12] = (crc >> 24) & 0xFF;
    sec[13] = (crc >> 16) & 0xFF;
    sec[14] = (crc >> 8) & 0xFF;
    sec[15] = crc & 0xFF;

    out.insert(out.end(), pkt, pkt + kTsSize);
}

void TsPacketizer::append_pmt(std::vector<uint8_t>& out) {
    uint8_t pkt[kTsSize];
    std::memset(pkt, 0xFF, kTsSize);
    pkt[0] = 0x47;
    pkt[1] = 0x40 | ((kPidPmt >> 8) & 0x1F);
    pkt[2] = kPidPmt & 0xFF;
    pkt[3] = 0x10 | (cc_pmt_++ & 0x0F);
    pkt[4] = 0x00; // pointer_field

    uint8_t* sec = pkt + 5;
    sec[0] = 0x02; // table_id = PMT
    sec[1] = 0xB0;
    sec[2] = 0x12; // section_length = 18 (对齐 K90 实测 PMT!)
    sec[3] = 0x00; // program_number = 1
    sec[4] = 0x01;
    sec[5] = 0xC3; // version=1, current_next=1
    sec[6] = 0x00;
    sec[7] = 0x00;
    sec[8] = 0xE0 | ((kPidPcr >> 8) & 0x1F);
    sec[9] = kPidPcr & 0xFF;
    sec[10] = 0xF0;
    sec[11] = 0x00; // program_info_length = 0
    sec[12] = static_cast<uint8_t>(stream_type_);
    sec[13] = 0xE0 | ((kPidAudio >> 8) & 0x1F);
    sec[14] = kPidAudio & 0xFF;
    sec[15] = 0xF0;
    sec[16] = 0x00; // ES_info_length = 0

    uint32_t crc = mpeg_crc32(sec, 17);
    sec[17] = (crc >> 24) & 0xFF;
    sec[18] = (crc >> 16) & 0xFF;
    sec[19] = (crc >> 8) & 0xFF;
    sec[20] = crc & 0xFF;

    out.insert(out.end(), pkt, pkt + kTsSize);
}

void TsPacketizer::append_pcr(std::vector<uint8_t>& out, uint64_t pcr_90khz) {
    uint8_t pkt[kTsSize];
    std::memset(pkt, 0xFF, kTsSize);
    pkt[0] = 0x47;
    pkt[1] = (kPidPcr >> 8) & 0x1F;
    pkt[2] = kPidPcr & 0xFF;
    pkt[3] = 0x20 | (cc_pcr_ & 0x0F); // adaptation field only (no CC increment)
    pkt[4] = 183;                    // adaptation_field_length
    pkt[5] = 0x10;                   // PCR_flag = 1

    uint64_t base = pcr_90khz & ((1ULL << 33) - 1);
    pkt[6] = static_cast<uint8_t>((base >> 25) & 0xFF);
    pkt[7] = static_cast<uint8_t>((base >> 17) & 0xFF);
    pkt[8] = static_cast<uint8_t>((base >> 9) & 0xFF);
    pkt[9] = static_cast<uint8_t>((base >> 1) & 0xFF);
    pkt[10] = static_cast<uint8_t>(((base & 1) << 7) | 0x7E);
    pkt[11] = 0x00;

    out.insert(out.end(), pkt, pkt + kTsSize);
}

std::vector<uint8_t> TsPacketizer::wrap_wfd_lpcm(const int16_t* interleaved_stereo, size_t frame_samples) {
    // Wi-Fi Display LPCM private header (4 bytes): sub_stream_id=0xA0, num_frames, emphasis/quant/freq/ch
    std::vector<uint8_t> out;
    out.reserve(4 + frame_samples * 4);
    out.push_back(0xA0);
    out.push_back(0x06);
    out.push_back(0x00);
    out.push_back(0x11); // 16-bit, 48kHz, 2ch
    for (size_t i = 0; i < frame_samples * 2; ++i) {
        uint16_t u = static_cast<uint16_t>(interleaved_stereo[i]);
        out.push_back(static_cast<uint8_t>((u >> 8) & 0xFF)); // Big-Endian
        out.push_back(static_cast<uint8_t>(u & 0xFF));
    }
    return out;
}

std::vector<uint8_t> TsPacketizer::packetize_audio_frame(
    const uint8_t* audio_payload,
    size_t payload_len,
    uint64_t pts_90khz,
    bool force_pat_pmt) {
    std::vector<uint8_t> out;
    out.reserve(kTsSize * 8);

    // 每 5 帧 (~100ms) 插入 PAT + PMT + PCR（与实机抓包周期完全一致）
    if (force_pat_pmt || (frame_counter_ % 5 == 0)) {
        append_pat(out);
        append_pmt(out);
        append_pcr(out, pts_90khz);
    }
    ++frame_counter_;

    // 构建 PES 头：对齐 K90 明文 ADTS AAC 格式 (flags=0x84 0x80, hdr_len=0x07, 5B PTS + 0xFF 0xFF)
    std::vector<uint8_t> pes;
    pes.reserve(16 + payload_len);
    pes.push_back(0x00);
    pes.push_back(0x00);
    pes.push_back(0x01);
    pes.push_back(stream_type_ == TsAudioStreamType::WfdLpcm ? 0xBD : 0xC0);

    const size_t pes_body_len = 3 + 7 + payload_len;
    const uint16_t pes_len_field = (pes_body_len <= 0xFFFF) ? static_cast<uint16_t>(pes_body_len) : 0;
    pes.push_back(static_cast<uint8_t>((pes_len_field >> 8) & 0xFF));
    pes.push_back(static_cast<uint8_t>(pes_len_field & 0xFF));
    pes.push_back(0x84); // data_alignment_indicator=1
    pes.push_back(0x80); // PTS_DTS_flags = 10 (PTS only)
    pes.push_back(0x07); // header_data_length = 7 (5B PTS + 2B stuffing)

    const uint64_t pts = pts_90khz & ((1ULL << 33) - 1);
    pes.push_back(static_cast<uint8_t>(0x21 | (((pts >> 30) & 0x07) << 1)));
    pes.push_back(static_cast<uint8_t>((pts >> 22) & 0xFF));
    pes.push_back(static_cast<uint8_t>(0x01 | (((pts >> 15) & 0x7F) << 1)));
    pes.push_back(static_cast<uint8_t>((pts >> 7) & 0xFF));
    pes.push_back(static_cast<uint8_t>(0x01 | ((pts & 0x7F) << 1)));
    pes.push_back(0xFF);
    pes.push_back(0xFF);

    pes.insert(pes.end(), audio_payload, audio_payload + payload_len);

    // 切分为 188 字节 TS 包
    size_t offset = 0;
    bool first = true;
    while (offset < pes.size()) {
        uint8_t pkt[kTsSize];
        pkt[0] = 0x47;
        pkt[1] = (first ? 0x40 : 0x00) | ((kPidAudio >> 8) & 0x1F);
        pkt[2] = kPidAudio & 0xFF;

        const size_t rem = pes.size() - offset;
        if (rem >= 184) {
            pkt[3] = 0x10 | (cc_audio_++ & 0x0F);
            std::memcpy(pkt + 4, pes.data() + offset, 184);
            offset += 184;
        } else {
            // 需要 adaptation field 填充
            const size_t pad = 184 - rem;
            pkt[3] = 0x30 | (cc_audio_++ & 0x0F);
            if (pad == 1) {
                pkt[4] = 0x00; // adaptation_field_length = 0
                std::memcpy(pkt + 5, pes.data() + offset, rem);
            } else {
                pkt[4] = static_cast<uint8_t>(pad - 1);
                pkt[5] = 0x00; // flags
                if (pad > 2) {
                    std::memset(pkt + 6, 0xFF, pad - 2);
                }
                std::memcpy(pkt + 4 + pad, pes.data() + offset, rem);
            }
            offset += rem;
        }
        out.insert(out.end(), pkt, pkt + kTsSize);
        first = false;
    }

    return out;
}

} // namespace miplay::media

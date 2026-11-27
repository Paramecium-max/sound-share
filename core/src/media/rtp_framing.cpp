#include "media/rtp_sender.h"

#include <algorithm>
#include <cstring>

namespace miplay::media {

namespace {
constexpr size_t kTsPktSize = 188;
constexpr size_t kMaxTsPerRtp = 7; // 7 * 188 = 1316 bytes < MTU 1500
} // namespace

size_t MultiAudioSender::append_rtp_batch(std::vector<uint8_t>& wire, const std::vector<uint8_t>& ts,
                                          uint16_t& sequence, uint32_t timestamp) {
    constexpr size_t chunk_max = kMaxTsPerRtp * kTsPktSize;
    size_t count = 0;
    size_t offset = 0;
    auto append = [&](size_t len) {
        auto packet = build_rtp_over_tcp_packet(sequence++, timestamp, ts.data() + offset,
                                                len, 0xDEADBEEFU, offset + len == ts.size());
        wire.insert(wire.end(), packet.begin(), packet.end());
        offset += len;
        ++count;
    };

    // Keep PSI/PCR in its own RTP frame when it would otherwise split the PES earlier.
    if (ts.size() > chunk_max && ts[0] == 0x47 && (ts[1] & 0x1F) == 0 && ts[2] == 0) {
        append(3 * kTsPktSize);
    }
    while (offset < ts.size()) append(std::min(chunk_max, ts.size() - offset));
    return count;
}

std::vector<uint8_t> MultiAudioSender::build_rtp_over_tcp_packet(
    uint16_t rtp_seq,
    uint32_t rtp_ts_90khz,
    const uint8_t* ts_bytes,
    size_t ts_len,
    uint32_t ssrc,
    bool marker) {
    const uint16_t rtp_len = static_cast<uint16_t>(12 + ts_len);
    std::vector<uint8_t> pkt(4 + rtp_len);
    pkt[0] = 0x24;
    pkt[1] = 0x00;
    pkt[2] = static_cast<uint8_t>((rtp_len >> 8) & 0xFF);
    pkt[3] = static_cast<uint8_t>(rtp_len & 0xFF);

    pkt[4] = 0x80;
    pkt[5] = marker ? 0xA1 : 0x21;
    pkt[6] = static_cast<uint8_t>((rtp_seq >> 8) & 0xFF);
    pkt[7] = static_cast<uint8_t>(rtp_seq & 0xFF);
    pkt[8] = static_cast<uint8_t>((rtp_ts_90khz >> 24) & 0xFF);
    pkt[9] = static_cast<uint8_t>((rtp_ts_90khz >> 16) & 0xFF);
    pkt[10] = static_cast<uint8_t>((rtp_ts_90khz >> 8) & 0xFF);
    pkt[11] = static_cast<uint8_t>(rtp_ts_90khz & 0xFF);
    pkt[12] = static_cast<uint8_t>((ssrc >> 24) & 0xFF);
    pkt[13] = static_cast<uint8_t>((ssrc >> 16) & 0xFF);
    pkt[14] = static_cast<uint8_t>((ssrc >> 8) & 0xFF);
    pkt[15] = static_cast<uint8_t>(ssrc & 0xFF);

    std::memcpy(pkt.data() + 16, ts_bytes, ts_len);
    return pkt;
}

} // namespace miplay::media

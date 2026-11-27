#include "media/rtp_sender.h"

namespace {
struct Packetizer {
    explicit Packetizer(miplay::media::TsAudioStreamType type) : ts(type) {}
    miplay::media::TsPacketizer ts;
    std::vector<uint8_t> wire;
};
}

extern "C" void* soundflow_packetizer_create(uint8_t stream) noexcept {
    switch (static_cast<miplay::media::TsAudioStreamType>(stream)) {
        case miplay::media::TsAudioStreamType::AdtsAac:
        case miplay::media::TsAudioStreamType::WfdLpcm:
            try { return new Packetizer(static_cast<miplay::media::TsAudioStreamType>(stream)); }
            catch (...) { return nullptr; }
        default: return nullptr;
    }
}

extern "C" void soundflow_packetizer_destroy(void* pointer) noexcept {
    delete static_cast<Packetizer*>(pointer);
}

extern "C" const uint8_t* soundflow_packetizer_frame(void* pointer, const uint8_t* audio, size_t size,
    uint64_t pts, uint16_t sequence, bool psi, size_t* wire_size) noexcept {
    if (!pointer || !audio || !size || size > 65525 || !wire_size) return nullptr;
    auto& packer = *static_cast<Packetizer*>(pointer);
    try {
        auto ts = packer.ts.packetize_audio_frame(audio, size, pts, psi);
        packer.wire.clear();
        miplay::media::MultiAudioSender::append_rtp_batch(packer.wire, ts, sequence, static_cast<uint32_t>(pts));
        *wire_size = packer.wire.size();
        return packer.wire.data();
    } catch (...) {
        return nullptr;
    }
}

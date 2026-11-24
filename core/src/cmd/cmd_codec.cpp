#include "cmd/cmd_codec.h"
#include "crypto/mini_crypto.h"

#include <cstring>

namespace miplay::cmd {

namespace {
constexpr uint8_t kFrameMagic = 0x24; // '$'
constexpr uint32_t kMaxPayload = 4 * 1024 * 1024;
} // namespace

std::vector<uint8_t> encode_frame(const CmdFrame& frame) {
    const uint32_t plen = static_cast<uint32_t>(frame.payload.size());
    std::vector<uint8_t> out;
    out.reserve(9 + plen);
    out.push_back(kFrameMagic);
    out.push_back(static_cast<uint8_t>((frame.cmd_type >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(frame.cmd_type & 0xFF));
    out.push_back(static_cast<uint8_t>((frame.seq >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(frame.seq & 0xFF));
    out.push_back(static_cast<uint8_t>((plen >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((plen >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((plen >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(plen & 0xFF));
    out.insert(out.end(), frame.payload.begin(), frame.payload.end());
    return out;
}

size_t decode_frames(const uint8_t* data, size_t len, std::vector<CmdFrame>& out_frames) {
    size_t off = 0;
    while (off + 9 <= len) {
        if (data[off] != kFrameMagic) {
            ++off;
            continue;
        }
        uint16_t cmd = (static_cast<uint16_t>(data[off + 1]) << 8) | data[off + 2];
        uint16_t seq = (static_cast<uint16_t>(data[off + 3]) << 8) | data[off + 4];
        uint32_t plen = (static_cast<uint32_t>(data[off + 5]) << 24) |
                        (static_cast<uint32_t>(data[off + 6]) << 16) |
                        (static_cast<uint32_t>(data[off + 7]) << 8) |
                        static_cast<uint32_t>(data[off + 8]);
        if (plen > kMaxPayload) {
            ++off;
            continue;
        }
        if (off + 9 + plen > len) {
            break;
        }
        CmdFrame f;
        f.cmd_type = cmd;
        f.seq = seq;
        f.payload.assign(data + off + 9, data + off + 9 + plen);
        out_frames.push_back(std::move(f));
        off += 9 + plen;
    }
    return off;
}

std::vector<uint8_t> compute_legacy_challenge_response(const std::vector<uint8_t>& challenge) {
    static const char kSeed[] = "0.0.0.0";
    auto md = crypto::md5(reinterpret_cast<const uint8_t*>(kSeed), 7);
    std::string key_hex = crypto::hex_lower(md.data(), md.size());
    auto mac = crypto::hmac_sha1(
        reinterpret_cast<const uint8_t*>(key_hex.data()), key_hex.size(),
        challenge.data(), challenge.size());
    std::string resp_hex = crypto::hex_lower(mac.data(), mac.size());
    return std::vector<uint8_t>(resp_hex.begin(), resp_hex.end());
}

std::string derive_type1_auth_key(const std::string& receiver_ip, uint16_t receiver_port,
                                  const std::string& sender_ip, uint16_t sender_port) {
    std::string raw = receiver_ip + std::to_string(receiver_port) +
                      sender_ip + std::to_string(sender_port);
    for (char& c : raw) {
        if (c >= '0' && c <= '9') c = static_cast<char>('a' + (c - '0'));
    }
    auto md = crypto::md5(reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
    return crypto::hex_lower(md.data(), md.size());
}

std::vector<uint8_t> encode_u32_be(uint32_t val) {
    return {
        static_cast<uint8_t>((val >> 24) & 0xFF),
        static_cast<uint8_t>((val >> 16) & 0xFF),
        static_cast<uint8_t>((val >> 8) & 0xFF),
        static_cast<uint8_t>(val & 0xFF)
    };
}

std::vector<uint8_t> encode_scalar_u32(uint32_t val) {
    return {
        0x00,
        static_cast<uint8_t>((val >> 24) & 0xFF),
        static_cast<uint8_t>((val >> 16) & 0xFF),
        static_cast<uint8_t>((val >> 8) & 0xFF),
        static_cast<uint8_t>(val & 0xFF)
    };
}

std::vector<uint8_t> encode_tlv_string(const std::string& key, const std::string& value) {
    std::vector<uint8_t> out;
    out.reserve(1 + key.size() + 1 + 4 + value.size());
    out.push_back(static_cast<uint8_t>(key.size()));
    out.insert(out.end(), key.begin(), key.end());
    out.push_back(0x1E);
    uint32_t vlen = static_cast<uint32_t>(value.size());
    out.push_back(static_cast<uint8_t>((vlen >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((vlen >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((vlen >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(vlen & 0xFF));
    out.insert(out.end(), value.begin(), value.end());
    return out;
}

std::optional<std::string> decode_tlv_string(const std::vector<uint8_t>& buf, std::string* out_key) {
    if (buf.size() < 6) return std::nullopt;
    uint8_t klen = buf[0];
    if (1U + klen + 5U > buf.size()) return std::nullopt;
    if (out_key) {
        out_key->assign(reinterpret_cast<const char*>(buf.data() + 1), klen);
    }
    size_t off = 1 + klen;
    if (buf[off++] != 0x1E) return std::nullopt;
    uint32_t vlen = (static_cast<uint32_t>(buf[off]) << 24) |
                    (static_cast<uint32_t>(buf[off + 1]) << 16) |
                    (static_cast<uint32_t>(buf[off + 2]) << 8) |
                    static_cast<uint32_t>(buf[off + 3]);
    off += 4;
    if (off + vlen > buf.size()) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(buf.data() + off), vlen);
}

void SafetyCipher::init(const uint8_t key16[16], const uint8_t iv16[16]) {
    std::memcpy(key_.data(), key16, 16);
    std::memcpy(iv_.data(), iv16, 16);
    ready_ = true;
}

std::vector<uint8_t> SafetyCipher::encrypt_blob(const std::vector<uint8_t>& plaintext) {
    if (!ready_) return plaintext;
    uint8_t pad_len = 0;
    auto ct = crypto::aes128_cbc_encrypt_zeropad(
        plaintext.data(), plaintext.size(), key_.data(), iv_.data(), &pad_len);
    uint32_t crc = crypto::crc32_mpeg2(ct.data(), ct.size());

    // 9B SafetyData Header: [00 07][ver=01][flags=E0][pad_len:u8][crc32_mpeg2:u32le] + ciphertext
    std::vector<uint8_t> blob;
    blob.reserve(9 + ct.size());
    blob.push_back(0x00);
    blob.push_back(0x07);
    blob.push_back(0x01);
    blob.push_back(0xE0);
    blob.push_back(pad_len);
    blob.push_back(static_cast<uint8_t>(crc & 0xFF));
    blob.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
    blob.push_back(static_cast<uint8_t>((crc >> 16) & 0xFF));
    blob.push_back(static_cast<uint8_t>((crc >> 24) & 0xFF));
    blob.insert(blob.end(), ct.begin(), ct.end());
    return blob;
}

std::optional<std::vector<uint8_t>> SafetyCipher::decrypt_blob(const std::vector<uint8_t>& blob) {
    if (!ready_) return blob;
    if (blob.size() < 25) return std::nullopt;
    uint16_t hlen = (static_cast<uint16_t>(blob[0]) << 8) | blob[1];
    uint8_t ver = blob[2];
    uint8_t flags = blob[3];
    uint8_t pad = blob[4];
    uint32_t expected_crc = static_cast<uint32_t>(blob[5]) |
                            (static_cast<uint32_t>(blob[6]) << 8) |
                            (static_cast<uint32_t>(blob[7]) << 16) |
                            (static_cast<uint32_t>(blob[8]) << 24);
    const uint8_t* ct = blob.data() + 9;
    const size_t ct_len = blob.size() - 9;
    if (hlen != 7 || ver != 1 || flags != 0xE0 || pad < 1 || pad > 16 || (ct_len % 16) != 0) {
        return std::nullopt;
    }
    if (crypto::crc32_mpeg2(ct, ct_len) != expected_crc) {
        return std::nullopt;
    }
    return crypto::aes128_cbc_decrypt_zeropad(ct, ct_len, pad, key_.data(), iv_.data());
}

} // namespace miplay::cmd

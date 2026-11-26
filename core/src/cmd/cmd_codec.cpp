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

namespace {

uint64_t read_be_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

uint64_t read_le_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (static_cast<uint64_t>(p[i]) << (8 * i));
    return v;
}

uint32_t read_be_u32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

std::string ascii_lower_str(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

void parse_notify_tlv_stream(const uint8_t* data, size_t len, std::vector<RemoteControlEvent>& out) {
    size_t off = 0;
    while (off + 2 <= len) {
        const uint8_t klen = data[off++];
        if (klen == 0 || off + klen + 1 > len) break;
        std::string key(reinterpret_cast<const char*>(data + off), klen);
        off += klen;
        const uint8_t vtype = data[off++];

        uint64_t int_val = 0;
        bool has_int = false;
        std::string str_val;

        if (vtype == 0x00 || vtype == 0x01 || vtype == 0x02 || vtype == 0x03) {
            if (off + 1 > len) break;
            int_val = data[off++];
            has_int = true;
        } else if (vtype == 0x04 || vtype == 0x05 || vtype == 0x06 || vtype == 0x07) {
            if (off + 4 <= len) {
                int_val = read_be_u32(data + off);
                off += 4;
                has_int = true;
            } else if (off + 2 <= len) {
                int_val = (static_cast<uint64_t>(data[off]) << 8) | data[off + 1];
                off += 2;
                has_int = true;
            } else {
                break;
            }
        } else if (vtype == 0x08 || vtype == 0x09) {
            if (off + 8 > len) break;
            int_val = read_be_u64(data + off);
            off += 8;
            has_int = true;
        } else if (vtype == 0x14 || vtype == 0x1E) {
            if (off + 4 > len) break;
            const uint32_t vlen = read_be_u32(data + off);
            off += 4;
            if (off + vlen > len) break;
            str_val.assign(reinterpret_cast<const char*>(data + off), vlen);
            off += vlen;
        } else if (vtype == 0x16) {
            if (off + 4 > len) break;
            const uint32_t vlen = read_be_u32(data + off);
            off += 4;
            if (off + vlen > len) break;
            parse_notify_tlv_stream(data + off, vlen, out);
            off += vlen;
            continue;
        } else {
            break;
        }

        const std::string k = ascii_lower_str(key);
        RemoteControlEvent ev;
        ev.raw_key = key;
        if (k == "key-pause" || k == "pause") {
            ev.action = RemoteControlAction::Pause;
            out.push_back(std::move(ev));
        } else if (k == "key-resume" || k == "key-play" || k == "resume" || k == "play") {
            ev.action = RemoteControlAction::Resume;
            out.push_back(std::move(ev));
        } else if (k == "key-next" || k == "next") {
            ev.action = RemoteControlAction::Next;
            out.push_back(std::move(ev));
        } else if (k == "key-prev" || k == "key-previous" || k == "prev" || k == "previous") {
            ev.action = RemoteControlAction::Previous;
            out.push_back(std::move(ev));
        } else if (k == "key-seek" || k == "seek") {
            ev.action = RemoteControlAction::Seek;
            if (has_int) {
                ev.position_ms = int_val;
            } else if (!str_val.empty()) {
                try { ev.position_ms = std::stoull(str_val); } catch (...) {}
            }
            out.push_back(std::move(ev));
        } else if (k == "volume") {
            int32_t vol = -1;
            if (has_int && int_val <= 100) {
                vol = static_cast<int32_t>(int_val);
            } else if (!str_val.empty()) {
                try {
                    int v = std::stoi(str_val);
                    if (v >= 0 && v <= 100) vol = v;
                } catch (...) {}
            }
            if (vol >= 0) {
                ev.action = RemoteControlAction::SetVolume;
                ev.volume = vol;
                out.push_back(std::move(ev));
            }
        }
    }
}

} // namespace

std::vector<RemoteControlEvent> decode_remote_control_events(uint16_t cmd_type,
                                                             const std::vector<uint8_t>& payload) {
    std::vector<RemoteControlEvent> out;
    if (cmd_type == static_cast<uint16_t>(CmdType::Notify)) {
        parse_notify_tlv_stream(payload.data(), payload.size(), out);
        return out;
    }
    if (cmd_type == static_cast<uint16_t>(CmdType::Pause)) {
        out.push_back(RemoteControlEvent{RemoteControlAction::Pause, 0, -1, "cmd-pause"});
        return out;
    }
    if (cmd_type == static_cast<uint16_t>(CmdType::Resume)) {
        out.push_back(RemoteControlEvent{RemoteControlAction::Resume, 0, -1, "cmd-resume"});
        return out;
    }
    if (cmd_type == static_cast<uint16_t>(CmdType::SetPosition) && payload.size() >= 8) {
        uint64_t le = read_le_u64(payload.data());
        uint64_t be = read_be_u64(payload.data());
        // 合理毫秒上限（24 小时 = 86,400,000 ms），兼容小端/大端实现
        constexpr uint64_t kMaxReasonableMs = 86400000ULL;
        uint64_t pos_ms = (le <= kMaxReasonableMs) ? le : ((be <= kMaxReasonableMs) ? be : le);
        out.push_back(RemoteControlEvent{RemoteControlAction::Seek, pos_ms, -1, "cmd-setposition"});
        return out;
    }
    return out;
}

std::vector<uint8_t> encode_notify_control_key(const std::string& key) {
    // 实机实证格式：[key_len:u8][key_ascii][0x00][0x01]
    std::vector<uint8_t> out;
    if (key.empty() || key.size() > 255) return out;
    out.reserve(key.size() + 3);
    out.push_back(static_cast<uint8_t>(key.size()));
    out.insert(out.end(), key.begin(), key.end());
    out.push_back(0x00);
    out.push_back(0x01);
    return out;
}

std::vector<uint8_t> encode_notify_seek_ms(uint64_t position_ms) {
    // 实机实证格式：[0x08]["key-seek"][0x09][position_ms:u64be]
    static constexpr char kKey[] = "key-seek";
    std::vector<uint8_t> out;
    out.reserve(18);
    out.push_back(8);
    out.insert(out.end(), kKey, kKey + 8);
    out.push_back(0x09);
    for (int i = 7; i >= 0; --i) {
        out.push_back(static_cast<uint8_t>((position_ms >> (i * 8)) & 0xFF));
    }
    return out;
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

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace miplay::cmd {

// 实机抓包 & 开源互操作实证的 TCP 8899 命令号
enum class CmdType : uint16_t {
    Open               = 0x0000,
    OpenAck            = 0x0001,
    Close              = 0x0002,
    CloseAck           = 0x0003,
    Pause              = 0x0004,
    PauseAck           = 0x0005,
    Resume             = 0x0006,
    ResumeAck          = 0x0007,
    SetVolume          = 0x000C,
    SetVolumeAck       = 0x000D,
    GetVolume          = 0x000E,
    GetVolumeAck       = 0x000F,
    GetPosition        = 0x0010,
    GetPositionAck     = 0x0011,
    SetMediaInfo       = 0x0012,
    SetMediaInfoAck    = 0x0013,
    GetMediaInfo       = 0x0014,
    GetMediaInfoAck    = 0x0015,
    HeartBeat          = 0x001A,
    HeartBeatAck       = 0x001B,
    GetState           = 0x001C,
    GetStateAck        = 0x001D,
    GetDeviceInfo      = 0x001E,
    GetDeviceInfoAck   = 0x001F,
    Notify             = 0x0022,
    Challenge          = 0x0028,
    ChallengeAck       = 0x0029,
    GetMirrorMode      = 0x0034,
    GetMirrorModeAck   = 0x0035,
    GetVersion         = 0x0036,
    GetVersionAck      = 0x0037,
    SetPlaySource      = 0x0040,
    SetPlaySourceAck   = 0x0041,
    SetPosition        = 0x0056,
    SetPositionAck     = 0x0057,
    SetLocalDeviceInfo = 0x0058,
    SetLocalDeviceInfoAck = 0x0059,
    SafetyInfo         = 0x1400,
    SafetyInfoAck      = 0x1401,
    SafetyAuth         = 0x1402,
    SafetyAuthAck      = 0x1403,
};

struct CmdFrame {
    uint16_t cmd_type = 0;
    uint16_t seq = 0;
    std::vector<uint8_t> payload;
};

// 状态化 AES-128-CBC 方向加密通道（严格对齐小米 SafetyData 9 字节头 + Zero Padding + CRC32-MPEG2）
class SafetyCipher {
public:
    void init(const uint8_t key16[16], const uint8_t iv16[16]);
    bool is_ready() const { return ready_; }

    std::vector<uint8_t> encrypt_blob(const std::vector<uint8_t>& plaintext);
    std::optional<std::vector<uint8_t>> decrypt_blob(const std::vector<uint8_t>& blob);

private:
    bool ready_ = false;
    std::array<uint8_t, 16> key_{};
    std::array<uint8_t, 16> iv_{};
};

// 9 字节定长帧头：[0x24][cmd:u16be][seq:u16be][plen:u32be]
std::vector<uint8_t> encode_frame(const CmdFrame& frame);
size_t decode_frames(const uint8_t* data, size_t len, std::vector<CmdFrame>& out_frames);

// Legacy 0x0028 -> 0x0029 认证应答：HMAC-SHA1(md5_hex("0.0.0.0"), challenge)
std::vector<uint8_t> compute_legacy_challenge_response(const std::vector<uint8_t>& challenge);

// Safety Type-1 密钥派生：md5_hex(tr(receiverIP + receiverPort + senderIP + senderPort, '0-9', 'a-j')) -> 32B ASCII
std::string derive_type1_auth_key(const std::string& receiver_ip, uint16_t receiver_port,
                                  const std::string& sender_ip, uint16_t sender_port);

// 4 字节大端无符号整数 [u32be]（用于 0x000C SetVolume：41% -> 00 00 00 29，兼容 SoundFlow 与小米全系接收端）
std::vector<uint8_t> encode_u32_be(uint32_t val);

// 5 字节大端标量 [0x00][u32be]
std::vector<uint8_t> encode_scalar_u32(uint32_t val);

// Safety 信封：[len(tag)][tag "cmd"/"ack"][0x1E][len:u32be][json]
std::vector<uint8_t> encode_tlv_string(const std::string& key, const std::string& value);
std::optional<std::string> decode_tlv_string(const std::vector<uint8_t>& buf, std::string* out_key = nullptr);

} // namespace miplay::cmd

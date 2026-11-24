#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace miplay::crypto {

// 零外部依赖标准哈希与编码（MD5 / SHA-1 / SHA-256 / HMAC / Base64 / Hex / CRC32-MPEG2）
std::array<uint8_t, 16> md5(const uint8_t* data, size_t len);
std::array<uint8_t, 20> sha1(const uint8_t* data, size_t len);
std::array<uint8_t, 32> sha256(const uint8_t* data, size_t len);

std::array<uint8_t, 20> hmac_sha1(const uint8_t* key, size_t key_len, const uint8_t* data, size_t len);
std::array<uint8_t, 32> hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* data, size_t len);

std::string hex_lower(const uint8_t* data, size_t len);
std::string base64_encode(const uint8_t* data, size_t len);
uint32_t crc32_mpeg2(const uint8_t* data, size_t len);

// 零外部依赖标准 AES-128-CBC (Zero-Padding 1..16B，严格对齐小米 MiPlay SafetyData 状态化 CBC 链)
// 调用成功后自动将 iv_inout 更新为最后一个密文块（16 字节）
std::vector<uint8_t> aes128_cbc_encrypt_zeropad(
    const uint8_t* plaintext,
    size_t len,
    const uint8_t key[16],
    uint8_t iv_inout[16],
    uint8_t* out_pad_len);

// 仅在解密且零填充校验通过时才更新 iv_inout
std::optional<std::vector<uint8_t>> aes128_cbc_decrypt_zeropad(
    const uint8_t* ciphertext,
    size_t cipher_len,
    uint8_t pad_len,
    const uint8_t key[16],
    uint8_t iv_inout[16]);

} // namespace miplay::crypto

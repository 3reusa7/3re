#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace pixel3d {

// SHA-256 (FIPS 180-4). Used to identify source images and cache inputs.
std::array<uint8_t, 32> sha256(std::span<const uint8_t> data);
std::string sha256_hex(std::span<const uint8_t> data);
std::string sha256_hex(std::string_view text);

}  // namespace pixel3d

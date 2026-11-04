#pragma once
// minisign signatures (format: https://jedisct1.github.io/minisign/), used for 3re's signed update
// manifests. Ed25519 (RFC 8032, SHA-512) and BLAKE2b-512 come from Monocypher 4.0.3. Qt-free: the
// updater and the tests use it too.
//
//   public key line:  base64("Ed" | key id (8) | Ed25519 public key (32))
//   signature line:   base64("ED" | key id (8) | Ed25519(BLAKE2b-512(file)) (64))   ("Ed": signs the file itself)
//   trusted comment:  "trusted comment: <text>"
//   global signature: base64(Ed25519(signature (64) | <text>))
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace threere::update {

using KeyId = std::array<std::uint8_t, 8>;
using Seed = std::array<std::uint8_t, 32>;

struct PublicKey {
  KeyId key_id{};
  std::array<std::uint8_t, 32> key{};
};

struct Signature {
  bool prehashed = true;  // "ED" (BLAKE2b-512 of the file) or "Ed" (the file itself)
  KeyId key_id{};
  std::array<std::uint8_t, 64> signature{};
  std::string trusted_comment;
  std::array<std::uint8_t, 64> global_signature{};
};

std::optional<std::vector<std::uint8_t>> base64_decode(std::string_view text);
std::string base64_encode(std::span<const std::uint8_t> data);
std::string key_id_hex(const KeyId& id);  // as minisign prints it (little-endian, upper case)

// A minisign .pub file, or its base64 line alone.
std::optional<PublicKey> parse_public_key(std::string_view text);
// A .minisig file (four lines).
std::optional<Signature> parse_signature(std::string_view text);

// Empty when both the signature and the trusted comment verify with key; otherwise the reason.
std::string verify(const PublicKey& key, const Signature& sig, std::span<const std::uint8_t> message);

// Signing for tests and release tools. The seed is the 32-byte Ed25519 secret.
PublicKey public_key_from_seed(const Seed& seed, const KeyId& key_id);
std::string public_key_file(const PublicKey& key);
std::string sign(const Seed& seed, const KeyId& key_id, std::span<const std::uint8_t> message,
                 std::string_view trusted_comment, std::string_view untrusted_comment = "signature from the 3re release key");

inline std::span<const std::uint8_t> bytes_of(std::string_view s) {
  return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

}  // namespace threere::update

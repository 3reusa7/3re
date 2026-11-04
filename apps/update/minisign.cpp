#include "update/minisign.hpp"

#include <cstdio>
#include <stdexcept>

extern "C" {
#include "monocypher-ed25519.h"
#include "monocypher.h"
}

namespace threere::update {
namespace {

constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr std::string_view kUntrusted = "untrusted comment: ";
constexpr std::string_view kTrusted = "trusted comment: ";

std::vector<std::string_view> lines_of(std::string_view text) {
  std::vector<std::string_view> out;
  while (!text.empty()) {
    const auto nl = text.find('\n');
    auto line = text.substr(0, nl);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    out.push_back(line);
    if (nl == std::string_view::npos) break;
    text.remove_prefix(nl + 1);
  }
  while (!out.empty() && out.back().empty()) out.pop_back();
  return out;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

std::array<std::uint8_t, 64> blake2b_512(std::span<const std::uint8_t> m) {
  std::array<std::uint8_t, 64> h{};
  crypto_blake2b(h.data(), h.size(), m.data(), m.size());
  return h;
}

}  // namespace

std::optional<std::vector<std::uint8_t>> base64_decode(std::string_view text) {
  text = trim(text);
  if (text.size() % 4 != 0) return std::nullopt;
  std::vector<std::uint8_t> out;
  out.reserve(text.size() / 4 * 3);
  for (std::size_t i = 0; i < text.size(); i += 4) {
    std::uint32_t v = 0;
    int pad = 0;
    for (std::size_t j = 0; j < 4; ++j) {
      const char c = text[i + j];
      if (c == '=') {
        // Padding only at the very end, at most two characters.
        if (i + 4 != text.size() || j < 2) return std::nullopt;
        ++pad;
        v <<= 6;
        continue;
      }
      if (pad) return std::nullopt;
      const auto p = kAlphabet.find(c);
      if (p == std::string_view::npos) return std::nullopt;
      v = (v << 6) | std::uint32_t(p);
    }
    out.push_back(std::uint8_t(v >> 16));
    if (pad < 2) out.push_back(std::uint8_t(v >> 8));
    if (pad < 1) out.push_back(std::uint8_t(v));
  }
  return out;
}

std::string base64_encode(std::span<const std::uint8_t> data) {
  std::string out;
  out.reserve((data.size() + 2) / 3 * 4);
  for (std::size_t i = 0; i < data.size(); i += 3) {
    const std::uint32_t b0 = data[i];
    const std::uint32_t b1 = i + 1 < data.size() ? data[i + 1] : 0;
    const std::uint32_t b2 = i + 2 < data.size() ? data[i + 2] : 0;
    const std::uint32_t v = (b0 << 16) | (b1 << 8) | b2;
    out.push_back(kAlphabet[(v >> 18) & 63]);
    out.push_back(kAlphabet[(v >> 12) & 63]);
    out.push_back(i + 1 < data.size() ? kAlphabet[(v >> 6) & 63] : '=');
    out.push_back(i + 2 < data.size() ? kAlphabet[v & 63] : '=');
  }
  return out;
}

std::string key_id_hex(const KeyId& id) {
  std::string out;
  char buf[3];
  for (int i = 7; i >= 0; --i) {
    std::snprintf(buf, sizeof buf, "%02X", id[std::size_t(i)]);
    out += buf;
  }
  return out;
}

std::optional<PublicKey> parse_public_key(std::string_view text) {
  const auto lines = lines_of(text);
  if (lines.empty() || lines.size() > 2) return std::nullopt;
  if (lines.size() == 2 && !lines[0].starts_with(kUntrusted)) return std::nullopt;
  const auto raw = base64_decode(lines.back());
  if (!raw || raw->size() != 42 || (*raw)[0] != 'E' || (*raw)[1] != 'd') return std::nullopt;
  PublicKey key;
  std::copy(raw->begin() + 2, raw->begin() + 10, key.key_id.begin());
  std::copy(raw->begin() + 10, raw->end(), key.key.begin());
  return key;
}

std::optional<Signature> parse_signature(std::string_view text) {
  const auto lines = lines_of(text);
  if (lines.size() != 4 || !lines[0].starts_with(kUntrusted) || !lines[2].starts_with(kTrusted)) return std::nullopt;
  const auto raw = base64_decode(lines[1]);
  const auto global = base64_decode(lines[3]);
  if (!raw || raw->size() != 74 || !global || global->size() != 64) return std::nullopt;
  if ((*raw)[0] != 'E' || ((*raw)[1] != 'D' && (*raw)[1] != 'd')) return std::nullopt;
  Signature sig;
  sig.prehashed = (*raw)[1] == 'D';
  std::copy(raw->begin() + 2, raw->begin() + 10, sig.key_id.begin());
  std::copy(raw->begin() + 10, raw->end(), sig.signature.begin());
  sig.trusted_comment = std::string(lines[2].substr(kTrusted.size()));
  std::copy(global->begin(), global->end(), sig.global_signature.begin());
  return sig;
}

std::string verify(const PublicKey& key, const Signature& sig, std::span<const std::uint8_t> message) {
  if (key.key_id != sig.key_id)
    return "signed with another key (" + key_id_hex(sig.key_id) + ", expected " + key_id_hex(key.key_id) + ")";
  int bad = 0;
  if (sig.prehashed) {
    const auto digest = blake2b_512(message);
    bad = crypto_ed25519_check(sig.signature.data(), key.key.data(), digest.data(), digest.size());
  } else {
    bad = crypto_ed25519_check(sig.signature.data(), key.key.data(), message.data(), message.size());
  }
  if (bad) return "the signature does not match the file";
  std::vector<std::uint8_t> global(sig.signature.begin(), sig.signature.end());
  global.insert(global.end(), sig.trusted_comment.begin(), sig.trusted_comment.end());
  if (crypto_ed25519_check(sig.global_signature.data(), key.key.data(), global.data(), global.size()))
    return "the trusted comment was changed after signing";
  return {};
}

PublicKey public_key_from_seed(const Seed& seed, const KeyId& key_id) {
  Seed copy = seed;
  std::array<std::uint8_t, 64> secret{};
  PublicKey key;
  key.key_id = key_id;
  crypto_ed25519_key_pair(secret.data(), key.key.data(), copy.data());
  crypto_wipe(secret.data(), secret.size());
  return key;
}

std::string public_key_file(const PublicKey& key) {
  std::vector<std::uint8_t> raw{'E', 'd'};
  raw.insert(raw.end(), key.key_id.begin(), key.key_id.end());
  raw.insert(raw.end(), key.key.begin(), key.key.end());
  return "untrusted comment: minisign public key " + key_id_hex(key.key_id) + "\n" + base64_encode(raw) + "\n";
}

std::string sign(const Seed& seed, const KeyId& key_id, std::span<const std::uint8_t> message,
                 std::string_view trusted_comment, std::string_view untrusted_comment) {
  for (const char c : trusted_comment)
    if (c == '\n' || c == '\r') throw std::invalid_argument("the trusted comment must be one line");
  for (const char c : untrusted_comment)
    if (c == '\n' || c == '\r') throw std::invalid_argument("the untrusted comment must be one line");
  Seed copy = seed;
  std::array<std::uint8_t, 64> secret{};
  std::array<std::uint8_t, 32> pub{};
  crypto_ed25519_key_pair(secret.data(), pub.data(), copy.data());
  const auto digest = blake2b_512(message);
  std::array<std::uint8_t, 64> sig{};
  crypto_ed25519_sign(sig.data(), secret.data(), digest.data(), digest.size());
  std::vector<std::uint8_t> global_msg(sig.begin(), sig.end());
  global_msg.insert(global_msg.end(), trusted_comment.begin(), trusted_comment.end());
  std::array<std::uint8_t, 64> global{};
  crypto_ed25519_sign(global.data(), secret.data(), global_msg.data(), global_msg.size());
  crypto_wipe(secret.data(), secret.size());

  std::vector<std::uint8_t> line{'E', 'D'};
  line.insert(line.end(), key_id.begin(), key_id.end());
  line.insert(line.end(), sig.begin(), sig.end());
  return std::string(kUntrusted) + std::string(untrusted_comment) + "\n" + base64_encode(line) + "\n" + std::string(kTrusted) +
         std::string(trusted_comment) + "\n" + base64_encode(global) + "\n";
}

}  // namespace threere::update

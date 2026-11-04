// Tests of the Qt-free update core (apps/update): minisign signatures (with the RFC 8032 / RFC 7693 vectors
// of the primitives underneath), release versions, the signed manifest, the choice of a release, the folder
// swap with the person's files carried over, and 3re-update.exe itself: an update from the previous
// release, and a roll-back when the new release fails its self-test.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <windows.h>

#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

#include "update/manifest.hpp"
#include "update/minisign.hpp"
#include "update/swap.hpp"
#include "update/version.hpp"

extern "C" {
#include "monocypher-ed25519.h"
#include "monocypher.h"
}

using namespace threere::update;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::vector<std::uint8_t> hex(std::string_view h) {
  std::vector<std::uint8_t> out;
  for (std::size_t i = 0; i + 1 < h.size(); i += 2) out.push_back(std::uint8_t(std::stoi(std::string(h.substr(i, 2)), nullptr, 16)));
  return out;
}

Seed test_seed(std::uint8_t fill) {
  Seed s;
  for (std::size_t i = 0; i < s.size(); ++i) s[i] = std::uint8_t(fill + i);
  return s;
}

const KeyId kTestId{1, 2, 3, 4, 5, 6, 7, 8};

fs::path scratch(const std::string& name) {
  const auto dir = fs::temp_directory_path() / "3re-update-tests" / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

void write(const fs::path& f, const std::string& text) {
  fs::create_directories(f.parent_path());
  std::ofstream(f, std::ios::binary) << text;
}

std::string read(const fs::path& f) {
  std::ifstream in(f, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// A release folder like package.ps1 makes: files plus RELEASE.json listing them.
void make_release(const fs::path& dir, const std::string& version, const std::vector<std::pair<std::string, std::string>>& files) {
  json list = json::array();
  for (const auto& [rel, text] : files) {
    write(dir / rel, text);
    list.push_back({{"path", rel}, {"size", text.size()}, {"sha256", std::string(64, '0')}});
  }
  write(dir / "RELEASE.json", json{{"product", "3re"}, {"version", version}, {"files", list}}.dump(1));
}

int run(const std::wstring& line) {
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  std::wstring cmd = line;
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
  WaitForSingleObject(pi.hProcess, 120000);
  DWORD code = 99;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return int(code);
}

std::wstring q(const fs::path& p) { return L"\"" + p.wstring() + L"\""; }

}  // namespace

TEST_CASE("primitives match RFC 8032 (Ed25519 test 1) and RFC 7693 (BLAKE2b-512 of \"abc\")") {
  auto seed = hex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
  std::uint8_t secret[64], pub[32], sig[64];
  crypto_ed25519_key_pair(secret, pub, seed.data());
  CHECK(std::vector<std::uint8_t>(pub, pub + 32) == hex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"));
  crypto_ed25519_sign(sig, secret, nullptr, 0);
  CHECK(std::vector<std::uint8_t>(sig, sig + 64) ==
        hex("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"));
  CHECK(crypto_ed25519_check(sig, pub, nullptr, 0) == 0);
  std::uint8_t h[64];
  crypto_blake2b(h, 64, reinterpret_cast<const std::uint8_t*>("abc"), 3);
  CHECK(std::vector<std::uint8_t>(h, h + 64) ==
        hex("ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d17d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923"));
}

TEST_CASE("base64 round trip and rejects") {
  const std::string text = "3re signs every release";
  const auto enc = base64_encode(bytes_of(text));
  const auto dec = base64_decode(enc);
  REQUIRE(dec);
  CHECK(std::string(dec->begin(), dec->end()) == text);
  CHECK(base64_encode(bytes_of("a")) == "YQ==");
  CHECK(!base64_decode("YQ="));
  CHECK(!base64_decode("Y=Q="));
  CHECK(!base64_decode("YQ*="));
}

TEST_CASE("minisign: sign, verify, and every way a signature must fail") {
  const auto seed = test_seed(7);
  const auto key = public_key_from_seed(seed, kTestId);
  const std::string manifest = R"({"format":"3re-update-manifest"})";
  const auto sig_text = sign(seed, kTestId, bytes_of(manifest), "3re update manifest 9.9.9");
  const auto sig = parse_signature(sig_text);
  REQUIRE(sig);
  CHECK(sig->prehashed);
  CHECK(sig->trusted_comment == "3re update manifest 9.9.9");
  CHECK(verify(key, *sig, bytes_of(manifest)).empty());

  // The public key file reads back the same key.
  const auto parsed = parse_public_key(public_key_file(key));
  REQUIRE(parsed);
  CHECK(parsed->key == key.key);
  CHECK(parsed->key_id == key.key_id);

  SUBCASE("a changed file") { CHECK(verify(key, *sig, bytes_of(manifest + " ")) == "the signature does not match the file"); }
  SUBCASE("a changed trusted comment") {
    auto s = *sig;
    s.trusted_comment = "3re update manifest 9.9.10";
    CHECK(verify(key, s, bytes_of(manifest)) == "the trusted comment was changed after signing");
  }
  SUBCASE("another key with the same key id") {
    const auto other = public_key_from_seed(test_seed(9), kTestId);
    CHECK(verify(other, *sig, bytes_of(manifest)) == "the signature does not match the file");
  }
  SUBCASE("another key id") {
    const auto other = public_key_from_seed(seed, KeyId{8, 7, 6, 5, 4, 3, 2, 1});
    CHECK(verify(other, *sig, bytes_of(manifest)).starts_with("signed with another key"));
  }
  SUBCASE("broken files") {
    CHECK(!parse_signature("untrusted comment: x\nnot base64\ntrusted comment: y\nAAAA\n"));
    CHECK(!parse_signature(sig_text.substr(0, sig_text.size() / 2)));
    CHECK(!parse_public_key("untrusted comment: x\nRWQ=\n"));
  }
}

TEST_CASE("minisign: a signature made by tools/release/minisign.mjs (Node) verifies here") {
  // Fixture made with a throw-away key whose secret was discarded (tests/update/fixtures/README.md).
  const fs::path dir = THREERE_UPDATE_FIXTURES;
  const auto key = parse_public_key(read(dir / "node-test.pub"));
  const auto sig = parse_signature(read(dir / "signed.txt.minisig"));
  REQUIRE(key);
  REQUIRE(sig);
  const auto message = read(dir / "signed.txt");
  CHECK(verify(*key, *sig, bytes_of(message)).empty());
  CHECK(!verify(*key, *sig, bytes_of(message + "x")).empty());
}

TEST_CASE("the built-in release key parses") {
  CHECK(parse_public_key("RWRQpxVGOWWmbuzzkmTFAKgBdhiYsfwb8QX105v54s6hvDPzmHOn2CRy"));
  const auto file = parse_public_key(read(fs::path(THREERE_SOURCE_DIR) / "tools/release/3re-update.pub"));
  REQUIRE(file);
  CHECK(key_id_hex(file->key_id) == "6EA665394615A750");
}

TEST_CASE("versions follow SemVer precedence") {
  const char* ordered[] = {"1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2",
                           "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0", "2.9.1-alpha", "2.10.0-alpha", "2.10.0"};
  for (std::size_t i = 0; i + 1 < std::size(ordered); ++i) {
    const auto a = parse_version(ordered[i]), b = parse_version(ordered[i + 1]);
    REQUIRE(a);
    REQUIRE(b);
    CHECK_MESSAGE(*a < *b, ordered[i], " < ", ordered[i + 1]);
    CHECK(!(*b < *a));
  }
  CHECK(parse_version("v2.9.1-alpha")->str() == "2.9.1-alpha");
  CHECK(*parse_version("2.9.1-alpha+build.5") == *parse_version("2.9.1-alpha"));
  for (const char* bad : {"2.9", "2.9.1-", "02.9.1", "2.9.1-01", "2.9.1.4", "x.y.z", "2.9.1-a..b", ""}) CHECK_MESSAGE(!parse_version(bad), bad);
}

namespace {
json manifest_json() {
  return {{"format", "3re-update-manifest"},
          {"format_version", 1},
          {"product", "3re"},
          {"version", "2.10.0-alpha"},
          {"channel", "alpha"},
          {"notes", {{"en", "Faster export."}, {"ar", "تصدير أسرع."}}},
          {"packages",
           {{{"platform", "windows-x64"},
             {"kind", "portable-zip"},
             {"name", "3re-2.10.0-alpha-windows-x64.zip"},
             {"url", "https://example.invalid/3re.zip"},
             {"size", 10},
             {"sha256", std::string(64, 'a')},
             {"files", {{{"path", "3re.exe"}, {"size", 3}, {"sha256", std::string(64, 'b')}}}}}}}};
}
}  // namespace

TEST_CASE("manifest: valid, and refused when unsafe or incomplete") {
  const auto m = parse_manifest(manifest_json().dump());
  CHECK(m.version_text == "2.10.0-alpha");
  CHECK(m.notes_ar == "تصدير أسرع.");
  REQUIRE(m.package_for("windows-x64"));
  CHECK(m.package_for("windows-x64")->files.size() == 1);
  CHECK(!m.package_for("linux-x64"));

  for (const char* path : {"../evil.dll", "/abs.exe", "a\\b.exe", "C:x.exe", "a//b", "./a", "a/../../b"}) {
    auto j = manifest_json();
    j["packages"][0]["files"][0]["path"] = path;
    CHECK_THROWS_AS(parse_manifest(j.dump()), std::runtime_error);
  }
  auto j = manifest_json();
  j["format_version"] = 2;
  CHECK_THROWS(parse_manifest(j.dump()));
  j = manifest_json();
  j["product"] = "other";
  CHECK_THROWS(parse_manifest(j.dump()));
  j = manifest_json();
  j["packages"][0]["sha256"] = "XYZ";
  CHECK_THROWS(parse_manifest(j.dump()));
  CHECK_THROWS(parse_manifest("not json"));
}

TEST_CASE("choose_release: channel, drafts, missing assets, older versions") {
  auto rel = [](const std::string& tag, bool pre, bool draft = false, bool assets = true) {
    json a = json::array();
    if (assets)
      a = {{{"name", "update-manifest.json"}, {"browser_download_url", "https://x/" + tag + "/m"}},
           {{"name", "update-manifest.json.minisig"}, {"browser_download_url", "https://x/" + tag + "/s"}}};
    return json{{"tag_name", tag}, {"prerelease", pre}, {"draft", draft}, {"assets", a}};
  };
  const json list = {rel("v2.9.0-alpha", true), rel("v2.10.0-alpha", true), rel("v3.0.0", false, true), rel("v2.11.0-alpha", true, false, false),
                     rel("v2.9.5", false), rel("not-a-version", false)};
  const auto current = *parse_version("2.9.1-alpha");
  const auto alpha = choose_release(list.dump(), current, "alpha");
  REQUIRE(alpha);
  CHECK(alpha->tag == "v2.10.0-alpha");  // v3.0.0 is a draft, v2.11.0-alpha has no manifest
  CHECK(alpha->manifest_url == "https://x/v2.10.0-alpha/m");
  const auto stable = choose_release(list.dump(), current, "stable");
  REQUIRE(stable);
  CHECK(stable->tag == "v2.9.5");
  CHECK(!choose_release(list.dump(), *parse_version("2.10.0"), "alpha"));
  CHECK(!choose_release("{}", current, "alpha"));
}

TEST_CASE("swap: the person's files move with the update, and back with a roll-back") {
  const auto root = scratch("swap");
  const auto install = root / "3re", staged = root / "stage", backup = root / "3re.previous", aside = root / "3re.failed";
  make_release(install, "2.9.1-alpha", {{"3re.exe", "old launcher"}, {"app/3re-studio.exe", "old studio"}, {"app/old-only.dll", "x"}});
  write(install / "models/trellis/q4/dinov3.gguf", "weights");        // downloaded on first run
  write(install / "app/runtime/ggml-cuda.dll", "engine");              // downloaded engine library
  write(install / "unins000.exe", "installer's uninstaller");
  make_release(staged, "2.10.0-alpha", {{"3re.exe", "new launcher"}, {"app/3re-studio.exe", "new studio"}, {"app/new-only.dll", "y"}});

  const auto r = swap_in(install, staged, backup);
  REQUIRE_MESSAGE(r.ok, r.message);
  CHECK(r.carried.size() == 3);
  CHECK(read(install / "3re.exe") == "new launcher");
  CHECK(read(install / "models/trellis/q4/dinov3.gguf") == "weights");
  CHECK(read(install / "app/runtime/ggml-cuda.dll") == "engine");
  CHECK(read(install / "unins000.exe") == "installer's uninstaller");
  CHECK(!fs::exists(install / "app/old-only.dll"));
  CHECK(read(backup / "3re.exe") == "old launcher");
  CHECK(!fs::exists(backup / "models/trellis/q4/dinov3.gguf"));
  CHECK(!fs::exists(staged));
  CHECK(fs::exists(install / kJournal));
  CHECK(release_version(install) == "2.10.0-alpha");

  const auto b = roll_back(install, backup, aside);
  REQUIRE_MESSAGE(b.ok, b.message);
  CHECK(read(install / "3re.exe") == "old launcher");
  CHECK(read(install / "app/old-only.dll") == "x");
  CHECK(read(install / "models/trellis/q4/dinov3.gguf") == "weights");
  CHECK(read(install / "unins000.exe") == "installer's uninstaller");
  CHECK(read(aside / "3re.exe") == "new launcher");
  CHECK(!fs::exists(aside / "models"));
  CHECK(!fs::exists(backup));
}

TEST_CASE("swap: refusals leave everything as it was") {
  const auto root = scratch("refuse");
  const auto install = root / "3re", staged = root / "stage", backup = root / "3re.previous";
  write(install / "3re.exe", "no RELEASE.json here");
  make_release(staged, "2.10.0-alpha", {{"3re.exe", "new"}});
  auto r = swap_in(install, staged, backup);
  CHECK(!r.ok);
  CHECK(read(install / "3re.exe") == "no RELEASE.json here");
  CHECK(fs::exists(staged / "3re.exe"));
  make_release(install, "2.9.1-alpha", {{"3re.exe", "old"}});
  fs::create_directories(backup);
  r = swap_in(install, staged, backup);
  CHECK(!r.ok);
  CHECK(read(install / "3re.exe") == "old");
  fs::remove_all(backup);
  r = swap_in(install, root / "missing", backup);
  CHECK(!r.ok);
  CHECK(read(install / "3re.exe") == "old");
}

namespace {
// An install made of the fake app, and a staged release whose self-test passes or fails.
void fake_release(const fs::path& dir, const std::string& version, bool selftest_passes) {
  fs::create_directories(dir / "app");
  fs::copy_file(FAKE_RELEASE_APP, dir / "3re.exe", fs::copy_options::overwrite_existing);
  write(dir / "selftest.txt", selftest_passes ? "pass" : "fail");
  write(dir / "app/version.txt", version);
  json list = json::array({{{"path", "3re.exe"}}, {{"path", "selftest.txt"}}, {{"path", "app/version.txt"}}});
  write(dir / "RELEASE.json", json{{"product", "3re"}, {"version", version}, {"files", list}}.dump());
}
}  // namespace

TEST_CASE("3re-update.exe: update from the previous release (self-test passes)") {
  const auto root = scratch("updater-ok");
  const auto install = root / "3re", staged = root / ".3re-update" / "stage-2.10.0-alpha", result = root / "result.json";
  fake_release(install, "2.9.1-alpha", true);
  write(install / "models/sam2.1-tiny/config.yaml", "user model");
  fake_release(staged, "2.10.0-alpha", true);
  const int code = run(q(UPDATER_EXE) + L" --install " + q(install) + L" --staged " + q(staged) + L" --result " + q(result));
  CHECK(code == 0);
  const auto out = json::parse(read(result));
  CHECK(out["status"] == "updated");
  CHECK(out["from"] == "2.9.1-alpha");
  CHECK(out["to"] == "2.10.0-alpha");
  CHECK(read(install / "app/version.txt") == "2.10.0-alpha");
  CHECK(read(install / "models/sam2.1-tiny/config.yaml") == "user model");
  CHECK(read(fs::path(install.wstring() + L".previous") / "app/version.txt") == "2.9.1-alpha");

  // ... and the person goes back with "Roll back" in Settings.
  const int back = run(q(UPDATER_EXE) + L" --rollback --install " + q(install) + L" --result " + q(result));
  CHECK(back == 0);
  CHECK(json::parse(read(result))["status"] == "rolled_back");
  CHECK(read(install / "app/version.txt") == "2.9.1-alpha");
  CHECK(read(install / "models/sam2.1-tiny/config.yaml") == "user model");
}

TEST_CASE("3re-update.exe: a release that fails its self-test is rolled back") {
  const auto root = scratch("updater-rollback");
  const auto install = root / "3re", staged = root / ".3re-update" / "stage-2.10.0-alpha", result = root / "result.json";
  fake_release(install, "2.9.1-alpha", true);
  write(install / "models/trellis/q4/ss_dec.gguf", "user model");
  fake_release(staged, "2.10.0-alpha", false);
  const int code = run(q(UPDATER_EXE) + L" --install " + q(install) + L" --staged " + q(staged) + L" --result " + q(result));
  CHECK(code == 1);
  const auto out = json::parse(read(result));
  CHECK(out["status"] == "rolled_back");
  CHECK(out["message"].get<std::string>().find("self-test") != std::string::npos);
  CHECK(read(install / "app/version.txt") == "2.9.1-alpha");
  CHECK(read(install / "models/trellis/q4/ss_dec.gguf") == "user model");
  CHECK(read(fs::path(install.wstring() + L".failed") / "app/version.txt") == "2.10.0-alpha");
}

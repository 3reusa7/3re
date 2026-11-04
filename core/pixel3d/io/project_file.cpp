#include "pixel3d/io/project_file.hpp"

#include <cmath>
#include <cstring>

#include <miniz.h>

#include "pixel3d/common/file_io.hpp"
#include "pixel3d/common/limits.hpp"
#include "pixel3d/common/path_utf8.hpp"
#include "pixel3d/common/sha256.hpp"
#include "pixel3d/common/version.hpp"
#include "pixel3d/engine/input_checks.hpp"
#include "pixel3d/io/json_io.hpp"
#include "pixel3d/io/json_safe.hpp"
#include "pixel3d/mesh/topology.hpp"

namespace pixel3d {
namespace {

constexpr char kFormat[] = "pixel3d.project";
constexpr uint32_t kMeshMagic = 0x4D443350;  // "P3DM"

template <class T>
void put(std::vector<uint8_t>& b, T v) {
  const auto* p = reinterpret_cast<const uint8_t*>(&v);
  b.insert(b.end(), p, p + sizeof(T));
}

template <class T>
bool get(std::span<const uint8_t> b, size_t& at, T* v) {
  if (at + sizeof(T) > b.size()) return false;
  std::memcpy(v, b.data() + at, sizeof(T));
  at += sizeof(T);
  return true;
}

// Bytes of a source image: the imported file, or a lossless re-encode if absent.
std::pair<std::string, std::shared_ptr<const std::vector<uint8_t>>> image_bytes(const SourceView& v) {
  if (v.original_bytes && !v.sha256.empty()) return {v.sha256, v.original_bytes};
  auto bytes = std::make_shared<const std::vector<uint8_t>>(encode_png(*v.original));
  return {sha256_hex(*bytes), bytes};
}

Message too_large(const std::string& what, uint64_t size, uint64_t limit) {
  return Message{"project.too_large",
                 what + " is too large (" + std::to_string(size) + " bytes, limit " + std::to_string(limit) + ")",
                 Severity::Error, {{"entry", what}, {"bytes", size}, {"limit", limit}}};
}

Message invalid(const std::string& text, nlohmann::json where = {}) {
  return Message{"project.invalid", text, Severity::Error, std::move(where)};
}

// Read-only view of a .p3d archive. The central directory is checked up front
// (entry count, declared sizes, total size, compression ratio, encryption), and
// every read is bounded by a per-entry limit before anything is decompressed.
class ZipReader {
 public:
  explicit ZipReader(std::vector<uint8_t> data) : data_(std::move(data)) {
    ok_ = mz_zip_reader_init_mem(&zip_, data_.data(), data_.size(), 0);
  }
  ~ZipReader() {
    if (ok_) mz_zip_reader_end(&zip_);
  }
  ZipReader(const ZipReader&) = delete;
  ZipReader& operator=(const ZipReader&) = delete;
  bool ok() const { return ok_; }

  std::optional<Message> check_directory() {
    const mz_uint n = mz_zip_reader_get_num_files(&zip_);
    if (n > limits::kMaxArchiveEntries)
      return invalid("the archive has " + std::to_string(n) + " entries (limit " +
                     std::to_string(limits::kMaxArchiveEntries) + ")");
    uint64_t total = 0;
    for (mz_uint i = 0; i < n; ++i) {
      mz_zip_archive_file_stat st;
      if (!mz_zip_reader_file_stat(&zip_, i, &st)) return invalid("the archive directory is damaged");
      if (st.m_is_encrypted || !st.m_is_supported)
        return invalid(std::string("entry uses encryption or an unsupported method: ") + st.m_filename);
      total += st.m_uncomp_size;
      if (total > limits::kMaxArchiveTotalBytes) return too_large("the archive content", total, limits::kMaxArchiveTotalBytes);
      if (st.m_uncomp_size > (1u << 20) &&
          st.m_uncomp_size / std::max<uint64_t>(1, st.m_comp_size) > limits::kMaxCompressionRatio)
        return invalid(std::string("entry has an implausible compression ratio: ") + st.m_filename);
    }
    return std::nullopt;
  }

  // Returns nullopt with *err unset when the entry does not exist.
  std::optional<std::vector<uint8_t>> read(const std::string& name, uint64_t max_bytes, std::optional<Message>* err) {
    const int idx = mz_zip_reader_locate_file(&zip_, name.c_str(), nullptr, 0);
    if (idx < 0) return std::nullopt;
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&zip_, static_cast<mz_uint>(idx), &st)) {
      *err = invalid("cannot read archive entry " + name);
      return std::nullopt;
    }
    if (st.m_uncomp_size > max_bytes) {
      *err = too_large(name, st.m_uncomp_size, max_bytes);
      return std::nullopt;
    }
    size_t size = 0;
    void* p = mz_zip_reader_extract_to_heap(&zip_, static_cast<mz_uint>(idx), &size, 0);  // CRC-checked
    if (!p || size != st.m_uncomp_size) {
      if (p) mz_free(p);
      *err = invalid("archive entry " + name + " is damaged");
      return std::nullopt;
    }
    std::vector<uint8_t> out(static_cast<uint8_t*>(p), static_cast<uint8_t*>(p) + size);
    mz_free(p);
    return out;
  }

 private:
  std::vector<uint8_t> data_;
  mz_zip_archive zip_{};
  bool ok_ = false;
};

std::string_view as_text(const std::vector<uint8_t>& b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }

bool is_sha256_hex(const std::string& s) {
  if (s.size() != 64) return false;
  for (char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

// A cached mesh is only trusted if its numbers are sane; topology is checked after.
bool mesh_cache_plausible(const Mesh& m) {
  for (const vec3d& p : m.positions)
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || std::abs(p.x) > 1e6 ||
        std::abs(p.y) > 1e6 || std::abs(p.z) > 1e6)
      return false;
  for (int part : m.tri_part)
    if (part < -1) return false;
  return !m.tris.empty();
}

std::optional<LogEntry> log_entry_from(std::string_view line) {
  const auto e = parse_json_untrusted(line, limits::kMaxLogBytes);
  if (!e || !e->is_object()) return std::nullopt;
  try {
    LogEntry le;
    le.time = e->value("time", "");
    const std::string src = e->value("source", "cli");
    le.source = src == "gui" ? Source::Gui : src == "mcp" ? Source::Mcp : Source::Cli;
    le.name = e->value("name", "");
    le.params = e->value("params", nlohmann::json());
    le.summary = e->value("summary", "");
    le.success = e->value("success", false);
    return le;
  } catch (const nlohmann::json::exception&) {
    return std::nullopt;  // the log is informational: damaged lines are skipped
  }
}

Expected<LoadedProject> load_project_impl(const std::filesystem::path& path) {
  auto bytes = read_file(path, limits::kMaxProjectFileBytes);
  if (!bytes) {
    Message m = bytes.error();
    if (m.code == "io.too_large") m.code = "project.too_large";
    return Unexpected{m};
  }
  ZipReader zip(std::move(*bytes));
  if (!zip.ok()) return make_error("project.invalid", "not a Pixel3D project (not a ZIP archive)", {{"path", to_utf8(path)}});
  if (auto e = zip.check_directory()) return Unexpected{*e};
  std::optional<Message> err;
  const auto pj_bytes = zip.read("project.json", limits::kMaxProjectJsonBytes, &err);
  if (err) return Unexpected{*err};
  if (!pj_bytes) return make_error("project.invalid", "project.json is missing", {{"path", to_utf8(path)}});
  auto parsed = parse_json_untrusted(as_text(*pj_bytes), limits::kMaxProjectJsonBytes);
  if (!parsed) return make_error("project.invalid", "project.json is not valid JSON: " + parsed.error().text);
  nlohmann::json pj = std::move(*parsed);
  if (!pj.is_object() || !pj.contains("format") || pj["format"] != kFormat || !pj.contains("format_version") ||
      !pj["format_version"].is_number_integer())
    return make_error("project.invalid", "project.json is not a Pixel3D project", {{"path", to_utf8(path)}});
  auto migrated = migrate_project_json(pj, pj["format_version"].get<int>());
  if (!migrated) return Unexpected{migrated.error()};
  pj = std::move(*migrated);
  if (!pj.contains("inputs") || !pj["inputs"].is_object()) return make_error("project.invalid", "project inputs are missing");

  std::map<std::string, ImageBlob> images;
  auto load_images = [&](const nlohmann::json& inputs) -> std::optional<Message> {
    if (!inputs.is_object()) return invalid("project inputs are malformed");
    const nlohmann::json views = inputs.value("views", nlohmann::json::array());
    if (!views.is_array() || views.size() > std::size(kAllViewRoles)) return invalid("project views are malformed");
    for (const auto& v : views) {
      if (!v.is_object() || !v.contains("sha256") || !v["sha256"].is_string()) return invalid("project view is malformed");
      const std::string sha = v["sha256"].get<std::string>();
      if (!is_sha256_hex(sha)) return invalid("project view has an invalid image id");
      if (images.contains(sha)) continue;
      // Format 3 names the entry after the image's format; older projects stored every image as .png.
      const std::string entry = v.value("image", "views/" + sha + ".png");
      if (!is_source_image_entry(entry, sha)) return invalid("project view names an invalid image entry", {{"entry", entry}});
      std::optional<Message> read_err;
      auto data = zip.read(entry, limits::kMaxImageFileBytes, &read_err);
      if (read_err) return read_err;
      if (!data || sha256_hex(*data) != sha)
        return invalid("source image " + sha + " is missing or damaged", {{"sha256", sha}});
      // The stored bytes are the imported file (any source format); its name tells a TGA apart.
      auto img = decode_image(*data, v.value("original_path", std::string()));
      if (!img) return img.error();
      images[sha] = ImageBlob{std::make_shared<const std::vector<uint8_t>>(std::move(*data)),
                              std::make_shared<const ImageRGBA>(std::move(*img))};
    }
    return std::nullopt;
  };
  auto load_inputs = [&](const nlohmann::json& inputs) -> Expected<Document> {
    if (auto e = load_images(inputs)) return Unexpected{*e};
    auto doc = inputs_from_json(inputs, images);
    if (!doc) return doc;
    if (auto e = check_document_inputs(*doc)) {
      Message m = *e;
      m.code = "project.invalid";
      m.text = "project contains an invalid value: " + m.text;
      return Unexpected{m};
    }
    return doc;
  };
  LoadedProject lp;
  auto doc = load_inputs(pj["inputs"]);
  if (!doc) return Unexpected{doc.error()};
  lp.doc = std::move(*doc);
  const nlohmann::json checkpoints = pj.value("checkpoints", nlohmann::json::object());  // keep alive while iterating
  if (!checkpoints.is_object()) return make_error("project.invalid", "project checkpoints are malformed");
  if (checkpoints.size() > static_cast<size_t>(limits::kMaxCheckpoints))
    return make_error("project.invalid", "too many checkpoints (limit " + std::to_string(limits::kMaxCheckpoints) + ")");
  for (const auto& [name, inputs] : checkpoints.items()) {
    if (name.empty() || name.size() > 256) return make_error("project.invalid", "checkpoint name is empty or too long");
    auto cp = load_inputs(inputs);
    if (!cp) return Unexpected{cp.error()};
    lp.checkpoints[name] = std::move(*cp);
  }
  // Mesh cache: only kept if it belongs to exactly these inputs and is sound.
  std::optional<Message> meta_err, mesh_err;
  const auto meta = zip.read("cache/meta.json", limits::kMaxMetaBytes, &meta_err);
  if (meta_err) return Unexpected{*meta_err};
  const auto mesh = meta ? zip.read("cache/mesh.bin", limits::kMaxMeshCacheBytes, &mesh_err) : std::nullopt;
  if (mesh_err) return Unexpected{*mesh_err};
  if (meta && mesh) {
    const auto mj = parse_json_untrusted(as_text(*meta), limits::kMaxMetaBytes);
    auto m = decode_mesh_cache(*mesh);
    if (mj && mj->is_object() && mj->contains("input_hash") && (*mj)["input_hash"].is_string() && m &&
        mesh_cache_plausible(*m) && (*mj)["input_hash"].get<std::string>() == mesh_input_hash(lp.doc)) {
      SolidResult s;
      s.mesh = std::move(*m);
      s.mesh.tri_region.clear();  // derived data: recomputed, never trusted from the file
      label_planar_regions(s.mesh);
      s.topology = analyze_topology(s.mesh);
      s.planar_regions = s.topology.planar_regions;
      if (s.topology.watertight && s.topology.nonmanifold_edges == 0 && s.topology.nonmanifold_vertices == 0) {
        lp.doc.cached_solid = std::make_shared<const SolidResult>(std::move(s));
        lp.doc.cached_input_hash = (*mj)["input_hash"].get<std::string>();
      }
    }
  }
  std::optional<Message> log_err;
  const auto log = zip.read("log.jsonl", limits::kMaxLogBytes, &log_err);
  if (log_err) return Unexpected{*log_err};
  if (log) {
    std::string_view rest = as_text(*log);
    while (!rest.empty() && lp.log.size() < static_cast<size_t>(limits::kMaxLogEntries)) {
      const size_t nl = rest.find('\n');
      const std::string_view line = rest.substr(0, nl);
      rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
      if (auto le = log_entry_from(line)) lp.log.push_back(std::move(*le));
    }
  }
  return lp;
}

}  // namespace

std::vector<uint8_t> encode_mesh_cache(const Mesh& m) {
  std::vector<uint8_t> b;
  put<uint32_t>(b, kMeshMagic);
  put<uint32_t>(b, 1);
  put<uint32_t>(b, static_cast<uint32_t>(m.positions.size()));
  put<uint32_t>(b, static_cast<uint32_t>(m.tris.size()));
  for (const vec3d& p : m.positions) put(b, p.x), put(b, p.y), put(b, p.z);
  for (size_t t = 0; t < m.tris.size(); ++t) {
    for (uint32_t i : m.tris[t]) put(b, i);
    put<int32_t>(b, t < m.tri_part.size() ? m.tri_part[t] : -1);
    put<int32_t>(b, t < m.tri_region.size() ? m.tri_region[t] : -1);
  }
  return b;
}

Expected<Mesh> decode_mesh_cache(std::span<const uint8_t> b) {
  size_t at = 0;
  uint32_t magic = 0, version = 0, nv = 0, nt = 0;
  if (!get(b, at, &magic) || magic != kMeshMagic || !get(b, at, &version) || version != 1 || !get(b, at, &nv) ||
      !get(b, at, &nt))
    return make_error("project.cache_invalid", "mesh cache header is invalid");
  // The declared counts must fit the data exactly (no allocation from a lying header).
  const uint64_t need = 16ull + uint64_t{nv} * 24 + uint64_t{nt} * 20;
  if (need != b.size()) return make_error("project.cache_invalid", "mesh cache size does not match its header");
  Mesh m;
  m.positions.reserve(nv);
  m.tris.reserve(nt);
  for (uint32_t i = 0; i < nv; ++i) {
    vec3d p;
    if (!get(b, at, &p.x) || !get(b, at, &p.y) || !get(b, at, &p.z)) return make_error("project.cache_invalid", "truncated");
    m.positions.push_back(p);
  }
  for (uint32_t t = 0; t < nt; ++t) {
    std::array<uint32_t, 3> tri;
    int32_t part = 0, region = 0;
    if (!get(b, at, &tri[0]) || !get(b, at, &tri[1]) || !get(b, at, &tri[2]) || !get(b, at, &part) || !get(b, at, &region))
      return make_error("project.cache_invalid", "truncated");
    for (uint32_t i : tri)
      if (i >= nv) return make_error("project.cache_invalid", "index out of range");
    m.tris.push_back(tri);
    m.tri_part.push_back(part);
    m.tri_region.push_back(region);
  }
  return m;
}

Message save_project(const Document& doc, const std::vector<LogEntry>& log, const History& history,
                     const std::filesystem::path& path) {
  // Every distinct source image (current and checkpoints), stored once by sha256.
  std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>> images;  // entry name → bytes
  auto inputs_with_images = [&](Document d) {
    for (SourceView& v : d.views) {
      auto [sha, bytes] = image_bytes(v);
      v.sha256 = sha;
      if (v.original_bytes != bytes) v.original_bytes = bytes;  // a re-encoded PNG
      images.emplace(source_image_entry(v), bytes);
    }
    return inputs_to_json(d);
  };
  nlohmann::json pj{{"format", kFormat}, {"format_version", kProjectFormatVersion}, {"app_version", version()}};
  pj["inputs"] = inputs_with_images(doc);
  pj["checkpoints"] = nlohmann::json::object();
  for (const auto& [name, cp] : history.checkpoints()) pj["checkpoints"][name] = inputs_with_images(cp);
  pj["validation"] = {{"status", doc.gate ? to_string(doc.gate->status) : "NOT_VALIDATED"}};

  const std::string js = pj.dump(2);
  // Never write a project this version could not load again.
  if (js.size() > limits::kMaxProjectJsonBytes || images.size() + 4 > limits::kMaxArchiveEntries)
    return Message{"project.too_large", "the project (with its checkpoints) exceeds the project file limits",
                   Severity::Error, {{"project_json_bytes", js.size()}, {"images", images.size()}}};
  mz_zip_archive zip{};
  if (!mz_zip_writer_init_heap(&zip, 0, 0)) return Message{"io.write_failed", "cannot create archive", Severity::Error, {}};
  auto add = [&](const std::string& name, const void* data, size_t size) {
    return mz_zip_writer_add_mem(&zip, name.c_str(), data, size, MZ_DEFAULT_COMPRESSION) != 0;
  };
  bool ok = add("project.json", js.data(), js.size());
  for (const auto& [entry, bytes] : images) ok = ok && add(entry, bytes->data(), bytes->size());
  const SolidResult* cache = doc.solid ? doc.solid.get() : nullptr;
  std::string cache_hash = doc.solid ? mesh_input_hash(doc) : "";
  if (!cache && doc.cached_solid && doc.cached_input_hash == mesh_input_hash(doc)) {
    cache = doc.cached_solid.get();
    cache_hash = doc.cached_input_hash;
  }
  if (cache) {
    const auto mesh = encode_mesh_cache(cache->mesh);
    const std::string meta = nlohmann::json{{"input_hash", cache_hash}, {"mode", to_string(doc.mode)}}.dump();
    ok = ok && add("cache/mesh.bin", mesh.data(), mesh.size()) && add("cache/meta.json", meta.data(), meta.size());
  }
  std::string lines;
  for (const LogEntry& e : log) lines += nlohmann::json(e).dump() + "\n";
  ok = ok && add("log.jsonl", lines.data(), lines.size());
  void* buf = nullptr;
  size_t size = 0;
  ok = ok && mz_zip_writer_finalize_heap_archive(&zip, &buf, &size);
  mz_zip_writer_end(&zip);
  if (!ok) {
    if (buf) mz_free(buf);
    return Message{"io.write_failed", "cannot build the project archive", Severity::Error, {}};
  }
  // Atomic replace (temp file + rename), so a crash never leaves a half-written project.
  const Message m = write_file_atomic(path, std::span<const uint8_t>(static_cast<const uint8_t*>(buf), size));
  mz_free(buf);
  return m;
}

Expected<LoadedProject> load_project(const std::filesystem::path& path) {
  try {
    return load_project_impl(path);
  } catch (const std::bad_alloc&) {
    return make_error("project.too_large", "not enough memory to load this project", {{"path", to_utf8(path)}});
  } catch (const std::exception& e) {
    return make_error("project.invalid", std::string("malformed project: ") + e.what(), {{"path", to_utf8(path)}});
  }
}

}  // namespace pixel3d

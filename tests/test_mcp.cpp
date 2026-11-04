// MCP protocol layer, in-process (the stdio end-to-end run is tests/mcp/e2e_client.py).
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <set>

#include "mcp/catalog.hpp"
#include "mcp/server.hpp"
#include "mcp/tools.hpp"
#include "pixel3d/polygon/contour.hpp"

using namespace pixel3d;
using namespace pixel3d::mcp;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct Harness {
  std::vector<json> out;
  Server server;
  explicit Harness(ServerOptions opt = {})
      : server(std::move(opt), [this](const std::string& line) {
          REQUIRE(line.find('\n') == std::string::npos);  // one message per line
          out.push_back(json::parse(line));
        }) {}

  json send(const json& msg) {
    out.clear();
    server.handle_line(msg.dump());
    REQUIRE_FALSE(out.empty());
    return out.back();
  }
  json init(const std::string& version = "2025-06-18") {
    return send({{"jsonrpc", "2.0"}, {"id", 0}, {"method", "initialize"}, {"params", {{"protocolVersion", version}}}});
  }
  json call(const std::string& tool, const json& args = json::object(), json meta = nullptr) {
    json params{{"name", tool}, {"arguments", args}};
    if (!meta.is_null()) params["_meta"] = meta;
    return send({{"jsonrpc", "2.0"}, {"id", 7}, {"method", "tools/call"}, {"params", params}});
  }
};

std::string fixture(const std::string& name, const std::string& role) {
  return std::string(PIXEL3D_FIXTURES_DIR) + "/" + name + "/" + role + ".png";
}

json fst(const std::string& name) {
  return {{"front", fixture(name, "front")}, {"side", fixture(name, "side")}, {"top", fixture(name, "top")}};
}

}  // namespace

TEST_CASE("mcp: lifecycle and version negotiation") {
  Harness h;
  json r = h.send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}});
  CHECK(r["error"]["code"] == rpc::kInvalidRequest);  // before initialize
  CHECK(h.send({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "ping"}})["result"] == json::object());
  r = h.init("1999-01-01");
  CHECK(r["result"]["protocolVersion"] == kSupportedProtocols[0]);  // unknown → newest
  r = h.init("2025-03-26");
  CHECK(r["result"]["protocolVersion"] == "2025-03-26");
  CHECK(r["result"].contains("instructions"));
  CHECK(r["result"]["serverInfo"]["name"] == "pixel3d-mcp");
  // Notifications are never answered.
  h.out.clear();
  h.server.handle_line(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
  CHECK(h.out.empty());
}

TEST_CASE("mcp: older protocol versions get only the fields they know") {
  Harness h;
  h.init("2024-11-05");
  const json tools = h.send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}})["result"]["tools"];
  CHECK_FALSE(tools[0].contains("annotations"));
  CHECK_FALSE(tools[0].contains("outputSchema"));
  const json r = h.call("inspect", {{"what", "project"}});
  CHECK_FALSE(r["result"].contains("structuredContent"));
  CHECK(json::parse(r["result"]["content"][0]["text"].get<std::string>())["success"] == true);
}

TEST_CASE("mcp: malformed input gets JSON-RPC errors, never a crash") {
  Harness h;
  h.init();
  h.out.clear();
  h.server.handle_line("{not json");
  CHECK(h.out.back()["error"]["code"] == rpc::kParseError);
  h.server.handle_line(std::string(10000, '[') + std::string(10000, ']'));
  CHECK(h.out.back()["error"]["code"] == rpc::kParseError);
  CHECK(h.send({{"jsonrpc", "1.0"}, {"id", 1}, {"method", "ping"}})["error"]["code"] == rpc::kInvalidRequest);
  CHECK(h.send({{"jsonrpc", "2.0"}, {"id", json::array()}, {"method", "ping"}})["error"]["code"] == rpc::kInvalidRequest);
  CHECK(h.send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}, {"params", 3}})["error"]["code"] == rpc::kInvalidParams);
  CHECK(h.send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "x/y"}})["error"]["code"] == rpc::kMethodNotFound);
  CHECK(h.send(json::array())["error"]["code"] == rpc::kInvalidRequest);
  const json r = h.call("inspect", json::array());
  CHECK(r["result"]["isError"] == true);
  CHECK(r["result"]["structuredContent"]["errors"][0]["code"] == "input.invalid");
}

TEST_CASE("mcp: tool schemas are complete and strict") {
  std::set<std::string> names;
  for (const ToolDef& t : tool_catalog()) {
    CAPTURE(t.name);
    CHECK(names.insert(t.name).second);
    CHECK_FALSE(t.description.empty());
    CHECK(t.input_schema["type"] == "object");
    CHECK(t.input_schema["additionalProperties"] == false);
    for (const auto& r : t.input_schema.value("required", json::array()))
      CHECK(t.input_schema["properties"].contains(r.get<std::string>()));
  }
  for (const char* n : {"new_project", "open_project", "save_project", "import_views", "auto_align", "analyze_views",
                        "segment_parts", "match_parts", "reconstruct", "get_conflicts", "get_inferred_geometry",
                        "set_part_depth", "edit_part", "cleanup_mesh", "project_textures", "validate", "render_view",
                        "render_turntable", "undo", "redo", "checkpoint", "restore_checkpoint", "export_glb"})
    CHECK(names.contains(n));
}

TEST_CASE("mcp: tool results carry the OpResult and inline images") {
  Harness h;
  h.init();
  json r = h.call("import_views", fst("A_synthetic_chair"));
  REQUIRE(r["result"]["isError"] == false);
  const json s = r["result"]["structuredContent"];
  for (const char* k : {"success", "errors", "warnings", "info", "changed", "changed_ids", "revision", "stats", "metrics",
                        "status", "validation", "data"})
    CHECK(s.contains(k));
  CHECK(s["status"] == "NOT_VALIDATED");
  // Progress notifications precede the response and increase.
  r = h.call("reconstruct", json::object(), {{"progressToken", "tok"}});
  REQUIRE(h.out.size() >= 2);
  double last = -1;
  for (size_t i = 0; i + 1 < h.out.size(); ++i) {
    CHECK(h.out[i]["method"] == "notifications/progress");
    CHECK(h.out[i]["params"]["progressToken"] == "tok");
    CHECK(h.out[i]["params"]["progress"].get<double>() > last);
    last = h.out[i]["params"]["progress"].get<double>();
  }
  r = h.call("render_view", {{"views", {"front"}}, {"pixels_per_unit", 2}});
  CHECK(r["result"]["structuredContent"]["errors"][0]["code"] == "state.not_ready");  // not textured yet
  REQUIRE(h.call("project_textures")["result"]["isError"] == false);
  r = h.call("render_view", {{"views", {"front"}}, {"pixels_per_unit", 2}});
  REQUIRE(r["result"]["isError"] == false);
  const json content = r["result"]["content"];
  REQUIRE(content.size() == 2);
  CHECK(content[1]["type"] == "image");
  CHECK(content[1]["mimeType"] == "image/png");
  CHECK(content[1]["data"].get<std::string>().rfind("iVBORw0KGgo", 0) == 0);  // PNG signature in base64
  CHECK(r["result"]["structuredContent"]["images"][0]["width"] == 36);
}

TEST_CASE("mcp: NEEDS_REVIEW is never exported silently") {
  Harness h;
  h.init();
  REQUIRE(h.call("import_views", fst("C_asym_planter"))["result"]["isError"] == false);
  const fs::path glb = fs::temp_directory_path() / "pixel3d_tests" / "mcp_c.glb";
  fs::remove(glb);
  json r = h.call("export_glb", {{"path", glb.string()}});
  CHECK(r["result"]["isError"] == true);
  CHECK(r["result"]["structuredContent"]["errors"][0]["code"] == "export.needs_review");
  CHECK_FALSE(fs::exists(glb));
  r = h.call("export_glb", {{"path", glb.string()}, {"accept_status", "NEEDS_REVIEW"}});
  CHECK(r["result"]["isError"] == false);
  CHECK(r["result"]["structuredContent"]["status"] == "NEEDS_REVIEW");
  CHECK(r["result"]["structuredContent"]["warnings"][0]["code"] == "export.status_accepted");
}

TEST_CASE("mcp: path roots restrict file access") {
  ServerOptions opt;
  opt.paths.roots = {fs::path(PIXEL3D_FIXTURES_DIR) / "A_synthetic_chair"};
  Harness h(opt);
  h.init();
  CHECK(h.call("import_views", fst("A_synthetic_chair"))["result"]["isError"] == false);
  const json r = h.call("import_views", {{"front", fixture("B_hand_chair", "front")}});
  CHECK(r["result"]["isError"] == true);
  CHECK(r["result"]["structuredContent"]["errors"][0]["text"].get<std::string>().find("--root") != std::string::npos);
  CHECK(h.call("save_project", {{"path", (fs::path(PIXEL3D_FIXTURES_DIR) / ".." / "x.p3d").string()}})["result"]["isError"] ==
        true);
}

TEST_CASE("mcp: a request cancelled before it starts is not answered") {
  Harness h;
  h.init();
  h.server.cancel(42);
  h.out.clear();
  h.server.handle_line(R"({"jsonrpc":"2.0","id":42,"method":"ping"})");
  CHECK(h.out.empty());
  // A cancellation for an id that was already answered does not affect a later reuse of it.
  h.server.handle_line(R"({"jsonrpc":"2.0","id":43,"method":"ping"})");
  h.server.cancel(43);
  h.server.handle_line(R"({"jsonrpc":"2.0","id":43,"method":"ping"})");
  CHECK(h.out.size() == 2);
  CHECK(Server::cancellation_target(R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":"a"}})") ==
        json("a"));
  CHECK_FALSE(Server::cancellation_target(R"({"jsonrpc":"2.0","id":1,"method":"ping"})"));
}

TEST_CASE("mcp: resources and prompts") {
  Harness h;
  h.init();
  json r = h.send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "resources/read"}, {"params", {{"uri", "pixel3d://views/front/aligned.png"}}}});
  CHECK(r["error"]["code"] == rpc::kResourceNotFound);  // nothing imported yet
  h.call("import_views", fst("A_synthetic_chair"));
  h.call("auto_align");
  r = h.send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "resources/read"}, {"params", {{"uri", "pixel3d://views/top/aligned.png"}}}});
  CHECK(r["result"]["contents"][0]["mimeType"] == "image/png");
  r = h.send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "resources/read"}, {"params", {{"uri", "pixel3d://views"}}}});
  const json views = json::parse(r["result"]["contents"][0]["text"].get<std::string>());
  CHECK(views["views"].size() == 3);
  CHECK(views["dims"]["W"] == 18);
  r = h.send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "prompts/get"}, {"params", {{"name", "nope"}}}});
  CHECK(r["error"]["code"] == rpc::kInvalidParams);
  CHECK(skill_markdown().find("name: pixel3d-reconstruct") != std::string_view::npos);
  CHECK(base64("") == "");
  CHECK(base64("f") == "Zg==");
  CHECK(base64("fo") == "Zm8=");
  CHECK(base64("foobar") == "Zm9vYmFy");
}

// ---- M4 editor: MCP ↔ core parity ------------------------------------------------

namespace {

struct Pair {
  Harness mcp;
  Session core;
  Pair() {
    mcp.init();
    REQUIRE(mcp.call("import_views", fst("A_synthetic_chair"))["result"]["isError"] == false);
    REQUIRE(mcp.call("project_textures")["result"]["isError"] == false);
    REQUIRE(core.import_views({{ViewRole::Front, fixture("A_synthetic_chair", "front"), {}},
                               {ViewRole::Side, fixture("A_synthetic_chair", "side"), {}},
                               {ViewRole::Top, fixture("A_synthetic_chair", "top"), {}}})
                .success);
    REQUIRE(core.project_textures(TextureOptions{}).success);
  }
  // Both sides must agree on the geometry and on the recorded edit.
  void same(const json& mcp_result, const OpResult& core_result, const char* what) {
    CAPTURE(what);
    const json s = mcp_result["result"]["structuredContent"];
    REQUIRE(s["success"] == true);
    REQUIRE(core_result.success);
    CHECK(s["stats"]["triangles"] == core_result.stats.triangles);
    CHECK(s["stats"]["volume"].get<double>() == doctest::Approx(core_result.stats.volume));
    if (core_result.data.contains("edit")) CHECK(s["data"]["edit"] == core_result.data["edit"]);
  }
};

}  // namespace

TEST_CASE("mcp parity: editing tools make the same core calls") {
  Pair p;
  p.same(p.mcp.call("paint_depth", {{"view", "top"}, {"stroke", {{6.5, 12.5}, {11.5, 12.5}}}, {"radius", 1.5},
                                    {"hardness", 1}, {"strength", 1}, {"mode", "lower"}}),
         [&] {
           DepthPaintRequest r;
           r.view = ViewRole::Top;
           r.stroke = {{6.5, 12.5}, {11.5, 12.5}};
           r.brush.radius = 1.5;
           r.brush.hardness = 1;
           r.brush.strength = 1;
           r.brush.raise = false;
           return p.core.paint_depth(r);
         }(),
         "paint_depth");
  p.same(p.mcp.call("push_pull", {{"view", "top"}, {"u", 9}, {"v", 15}, {"distance", 1}}),
         [&] {
           PushPullRequest r;
           r.view = ViewRole::Top;
           r.u = 9;
           r.v = 15;
           r.distance = 1;
           return p.core.push_pull(r);
         }(),
         "push_pull");
  p.same(p.mcp.call("extrude", {{"view", "front"}, {"rect", {6, 8, 12, 12}}, {"distance", 1}}),
         p.core.extrude(ViewRole::Front, rect_polys(6, 8, 12, 12), 1, -1), "extrude");
  const int leg = p.core.select_at_pixel(ViewRole::Front, 1, 30, SelectMode::Part).data["hit"]["part"].get<int>();
  const json sel = p.mcp.call("set_selection", {{"view", "front"}, {"u", 1}, {"v", 30}, {"mode", "part"}});
  CHECK(sel["result"]["structuredContent"]["data"]["parts"] == json::array({leg}));
  CHECK(p.mcp.call("get_selection")["result"]["structuredContent"]["data"]["views"] ==
        p.core.get_selection().data["views"]);
  p.same(p.mcp.call("mirror", {{"part", leg}, {"axis", "z"}, {"at", 21}, {"copy", true}}),
         p.core.mirror(leg, 2, 21.0, true), "mirror");
  p.same(p.mcp.call("duplicate_part", {{"part", leg}, {"offset", {0, 0, -4}}}),
         [&] {
           TransformRequest r;
           r.part = leg;
           r.translate = {0, 0, -4};
           r.copy = true;
           return p.core.transform_part(r);
         }(),
         "duplicate_part");
  p.same(p.mcp.call("split_part", {{"part", leg}, {"by", "plane"}, {"axis", "y"}, {"at", 7}}),
         p.core.split_part(leg, 1, 7.0), "split_part");
  p.same(p.mcp.call("add_volume", {{"view", "front"}, {"rect", {4, 20, 14, 22}}, {"depth_from", 2}, {"depth_to", 4}}),
         p.core.add_volume(ViewRole::Front, rect_polys(4, 20, 14, 22), 2, 4, -1), "add_volume");
  p.same(p.mcp.call("erase_volume", {{"view", "top"}, {"rect", {0, 18, 18, 20}}, {"depth_from", -1}, {"depth_to", 40}}),
         p.core.erase_volume(ViewRole::Top, rect_polys(0, 18, 18, 20), -1, 40, -1), "erase_volume");
  p.same(p.mcp.call("cut", {{"axis", "y"}, {"at", 30}, {"keep", "negative"}}), p.core.cut(-1, 1, 30.0, -1), "cut");
  // Parts and masks.
  const json c1 = p.mcp.call("create_part", {{"name", "tab"}});
  const OpResult c2 = p.core.create_part("tab", false);
  CHECK(c1["result"]["structuredContent"]["data"]["part"] == c2.data["part"]);
  const int id = c2.data["part"].get<int>();
  p.same(p.mcp.call("paint_part_mask", {{"part", id}, {"view", "top"}, {"rect", {6, 8, 12, 16}}}),
         [&] {
           MaskPaintRequest r;
           r.part = id;
           r.view = ViewRole::Top;
           for (int v = 8; v < 16; ++v)
             for (int u = 6; u < 12; ++u) r.pixels.emplace_back(u, v);
           return p.core.paint_part_mask(r);
         }(),
         "paint_part_mask");
  // The undo history holds the same steps on both sides.
  CHECK(p.mcp.server.session().document().edits.size() == p.core.document().edits.size());
  CHECK(p.mcp.server.session().document().masks == p.core.document().masks);
  const json undo = p.mcp.call("undo");
  CHECK(undo["result"]["isError"] == false);
  REQUIRE(p.core.undo().success);
  CHECK(p.mcp.server.session().document().masks == p.core.document().masks);
}

TEST_CASE("mcp: editor resources and errors") {
  Harness h;
  h.init();
  h.call("import_views", fst("A_synthetic_chair"));
  json r = h.call("push_pull", {{"face", 0}, {"distance", 1}});  // nothing reconstructed yet → reconstructs, face 0 exists
  CHECK(r["result"]["structuredContent"].contains("success"));
  r = h.call("merge_parts", {{"parts", {1}}});
  CHECK(r["result"]["isError"] == true);
  r = h.call("paint_part_mask", {{"part", 5}, {"view", "front"}, {"pixels", {{1, 1}}}});
  CHECK(r["result"]["structuredContent"]["errors"][0]["code"] == "edit.unknown_part");
  r = h.call("paint_depth", {{"view", "top"}});
  CHECK(r["result"]["structuredContent"]["errors"][0]["code"] == "input.invalid");
  for (const char* uri : {"pixel3d://edits", "pixel3d://live", "pixel3d://selection"}) {
    const json rr = h.send({{"jsonrpc", "2.0"}, {"id", 9}, {"method", "resources/read"}, {"params", {{"uri", uri}}}});
    CAPTURE(uri);
    CHECK(rr.contains("result"));
  }
  r = h.call("new_single_view_project", {{"path", fixture("B_hand_chair", "front")}, {"preset", "thin"}});
  CHECK(r["result"]["isError"] == false);
  r = h.call("validate");
  CHECK(r["result"]["structuredContent"]["status"] != "NOT_VALIDATED");
  r = h.call("inspect", {{"what", "edits"}});
  CHECK(r["result"]["structuredContent"]["data"]["single_view"]["enabled"] == true);
}

TEST_CASE("mcp parity: export_model writes the same files as the core command") {
  const fs::path dir = fs::temp_directory_path() / "pixel3d_mcp_export";
  fs::remove_all(dir);
  fs::create_directories(dir / "mcp");
  fs::create_directories(dir / "core");
  Pair p;
  const json r = p.mcp.call("export_model", {{"path", (dir / "mcp" / "chair.obj").string()}, {"preset", "roblox"}});
  REQUIRE(r["result"]["isError"] == false);
  const json s = r["result"]["structuredContent"];
  CHECK(s["data"]["format"] == "obj");
  CHECK(s["data"]["units"] == "studs");
  CHECK(s["data"]["front"] == "-z");
  ModelExportRequest m;
  m.path = dir / "core" / "chair.obj";
  m.preset = EnginePreset::Roblox;
  REQUIRE(p.core.export_model(m).success);
  for (const char* f : {"chair.obj", "chair.mtl", "chair.png"}) {
    CAPTURE(f);
    auto read = [](const fs::path& path) {
      std::ifstream in(path, std::ios::binary);
      std::stringstream ss;
      ss << in.rdbuf();
      return ss.str();
    };
    const std::string sa = read(dir / "mcp" / f), sb = read(dir / "core" / f);
    CHECK(!sa.empty());
    CHECK(sa == sb);
  }
  const json bad = p.mcp.call("export_model", {{"path", (dir / "x.glb").string()}, {"units", "furlongs"}});
  CHECK(bad["result"]["isError"] == true);
  CHECK(bad["result"]["structuredContent"]["errors"][0]["code"] == "input.invalid");
  fs::remove_all(dir);
}

TEST_CASE("mcp: normalisation settings in new_project, import_views and update_settings") {
  Harness h;
  h.init();
  json r = h.call("new_project", {{"name", "n"}, {"normalize", false}, {"target_size", 96}, {"bg_tolerance", 30},
                                  {"axis_tolerance", 0.2}});
  REQUIRE(r["result"]["isError"] == false);
  json p = h.call("inspect", {{"what", "project"}})["result"]["structuredContent"]["data"];
  CHECK(p["settings"]["normalize"]["enabled"] == false);
  CHECK(p["settings"]["normalize"]["target_size"] == 96);
  CHECK(p["settings"]["normalize"]["bg_tolerance"] == 30);
  CHECK(p["settings"]["normalize"]["axis_tolerance"] == doctest::Approx(0.2));

  json views = fst("A_synthetic_chair");
  views["target_size"] = 64;
  views["normalize"] = true;
  REQUIRE(h.call("import_views", views)["result"]["isError"] == false);
  p = h.call("inspect", {{"what", "project"}})["result"]["structuredContent"]["data"];
  CHECK(p["settings"]["normalize"]["enabled"] == true);
  CHECK(p["settings"]["normalize"]["target_size"] == 64);

  REQUIRE(h.call("update_settings", {{"bg_tolerance", 10}})["result"]["isError"] == false);
  p = h.call("inspect", {{"what", "project"}})["result"]["structuredContent"]["data"];
  CHECK(p["settings"]["normalize"]["bg_tolerance"] == 10);

  for (const json& bad : {json{{"target_size", 4}}, json{{"bg_tolerance", 300}}, json{{"axis_tolerance", 2}},
                          json{{"normalize", "yes"}}}) {
    const json e = h.call("update_settings", bad);
    CHECK(e["result"]["isError"] == true);
    CHECK(e["result"]["structuredContent"]["errors"][0]["code"] == "input.invalid");
  }
  // Clean pixel art: aligned as before, nothing reported as normalised.
  r = h.call("auto_align");
  REQUIRE(r["result"]["isError"] == false);
  CHECK(r["result"]["structuredContent"]["data"]["views"]["front"]["normalization"]["applied"] == false);
  // The tool schemas list the new arguments.
  const json list = h.send({{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/list"}})["result"]["tools"];
  for (const json& t : list)
    if (t["name"] == "new_project" || t["name"] == "import_views" || t["name"] == "update_settings")
      for (const char* k : {"normalize", "target_size", "bg_tolerance", "axis_tolerance"})
        CHECK(t["inputSchema"]["properties"].contains(k));
}

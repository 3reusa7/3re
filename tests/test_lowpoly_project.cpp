#include <doctest/doctest.h>

#include "pixel3d/io/json_io.hpp"
#include "pixel3d/io/migrate.hpp"
#include "pixel3d/lowpoly/project_settings.hpp"

using namespace pixel3d;

// Project format 5: LOW-POLY STUDIO settings (inputs.lowpoly) and part types (inputs.part_types).

TEST_CASE("lowpoly project: format 5 is current and 4 migrates with empty defaults") {
  CHECK(kProjectFormatVersion == 5);
  nlohmann::json inputs{{"settings", {{"name", "x"}, {"style", {{"preset", "soft"}}}}}, {"views", nlohmann::json::array()}};
  nlohmann::json j{{"format", "pixel3d.project"}, {"format_version", 4}, {"inputs", inputs},
                   {"checkpoints", {{"cp", inputs}}}};
  const auto m = migrate_project_json(j, 4);
  REQUIRE(m);
  CHECK((*m)["format_version"] == 5);
  for (const nlohmann::json* in : {&(*m)["inputs"], &(*m)["checkpoints"]["cp"]}) {
    CHECK((*in)["lowpoly"] == nlohmann::json{{"enabled", false}});
    CHECK((*in)["part_types"] == nlohmann::json::object());
    CHECK((*in)["settings"]["style"]["preset"] == "soft");  // earlier steps untouched
  }
}

TEST_CASE("lowpoly project: format 1 migrates through every step to 5") {
  nlohmann::json j{{"format", "pixel3d.project"}, {"format_version", 1},
                   {"inputs", {{"settings", {{"name", "old"}}}, {"views", nlohmann::json::array()}}}};
  const auto m = migrate_project_json(j, 1);
  REQUIRE(m);
  const auto& in = (*m)["inputs"];
  CHECK(in.contains("edits"));
  CHECK(in["settings"].contains("normalize"));
  CHECK(in["settings"]["style"]["preset"] == "crisp");
  CHECK(in["lowpoly"]["enabled"] == false);
  CHECK(in["part_types"].is_object());
}

TEST_CASE("lowpoly project: settings and part types round-trip through the project inputs") {
  Document d;
  d.settings.lowpoly.enabled = true;
  d.settings.lowpoly.build.tri_budget = 1500;
  d.settings.lowpoly.build.joint_loops = 3;
  d.settings.lowpoly.build.style = lowpoly::Style::Cel;
  d.settings.lowpoly.build.tones = 2;
  d.settings.lowpoly.build.templ = lowpoly::Template::CloakedCharacter;
  d.settings.lowpoly.build.baked_cel = true;
  d.settings.lowpoly.build.depth = 24;
  d.settings.lowpoly.build.user_depth = true;
  d.settings.lowpoly.build.accept_inferred_depth = true;
  d.settings.lowpoly.depth_ratio[1001] = 0.35;
  d.part_types[1000] = lowpoly::PartType::Head;
  d.part_types[1001] = lowpoly::PartType::Accessory;
  const nlohmann::json j = inputs_to_json(d);
  CHECK(j["lowpoly"]["enabled"] == true);
  CHECK(j["part_types"]["1000"] == "head");
  const auto back = inputs_from_json(j, {});
  REQUIRE(back);
  CHECK(back->settings.lowpoly == d.settings.lowpoly);
  CHECK(back->part_types == d.part_types);
}

TEST_CASE("lowpoly project: a format 4 project's inputs load as a standard project") {
  nlohmann::json j = inputs_to_json(Document{});
  j.erase("lowpoly");
  j.erase("part_types");
  const auto d = inputs_from_json(j, {});
  REQUIRE(d);
  CHECK_FALSE(d->settings.lowpoly.enabled);
  CHECK(d->part_types.empty());
  CHECK(d->settings.lowpoly.build.tri_budget == 1000);
}

TEST_CASE("lowpoly project: the crisp and polished cache keys ignore the low-poly inputs") {
  Document d;
  const std::string mesh = mesh_input_hash(d), style = style_input_hash(d);
  d.settings.lowpoly.enabled = true;
  d.settings.lowpoly.build.tri_budget = 700;
  d.part_types[1000] = lowpoly::PartType::Limb;
  CHECK(mesh_input_hash(d) == mesh);
  CHECK(style_input_hash(d) == style);
}

TEST_CASE("lowpoly project: invalid settings and types are refused") {
  auto parse = [](nlohmann::json j) { return lowpoly_settings_from_json(j); };
  CHECK(parse({{"enabled", true}}));
  CHECK_FALSE(parse({{"tri_budget", 100}}));
  CHECK_FALSE(parse({{"tri_budget", 9000}}));
  CHECK_FALSE(parse({{"joint_loops", 0}}));
  CHECK_FALSE(parse({{"tones", 4}}));
  CHECK_FALSE(parse({{"style", "glossy"}}));
  CHECK_FALSE(parse({{"template", "robot"}}));
  CHECK_FALSE(parse({{"depth", -1}}));
  CHECK_FALSE(parse({{"depth_ratio", {{"1000", 5.0}}}}));
  CHECK_FALSE(parse("not an object"));
  CHECK(part_types_from_json({{"1000", "head"}, {"1001", "cloth"}}));
  CHECK_FALSE(part_types_from_json({{"1000", "wing"}}));
  CHECK_FALSE(part_types_from_json({{"x", "head"}}));
  nlohmann::json bad = inputs_to_json(Document{});
  bad["lowpoly"]["tri_budget"] = 3;
  const auto d = inputs_from_json(bad, {});
  REQUIRE_FALSE(d);
  CHECK(d.error().code == "project.invalid");
}

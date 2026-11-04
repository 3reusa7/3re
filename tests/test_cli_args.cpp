#include <doctest/doctest.h>

#include "cli/cli_args.hpp"

using namespace pixel3d;

TEST_CASE("cli: args parse reconstruct with all views") {
  const auto a = parse_cli({"reconstruct", "--front", "f.png", "--side", "s.png", "--top", "t.png", "--back", "b.png",
                            "--right", "r.png", "--bottom", "u.png", "--mode", "voxel", "--profile", "strict",
                            "--project", "p.p3d", "--out", "o.glb", "--report", "rep.json", "--renders", "rd",
                            "--turntable", "--pixels-per-meter", "16"});
  REQUIRE(a.has_value());
  CHECK(a->command == "reconstruct");
  CHECK(a->views.size() == 6);
  CHECK(a->views.at(ViewRole::Bottom) == "u.png");
  CHECK(a->mode == ReconstructMode::Voxel);
  CHECK(a->profile == ToleranceProfile::Strict);
  CHECK(a->project == "p.p3d");
  CHECK(a->out == "o.glb");
  CHECK(a->report == "rep.json");
  CHECK(a->renders == "rd");
  CHECK(a->turntable);
  CHECK(a->pixels_per_meter == 16);
}

TEST_CASE("cli: missing --out is a usage error (exit 1)") {
  const auto a = parse_cli({"reconstruct", "--front", "f.png", "--side", "s.png", "--top", "t.png"});
  REQUIRE_FALSE(a.has_value());
  CHECK(a.error().code == "cli.usage");
}

TEST_CASE("cli: missing required view and unknown flag are usage errors") {
  CHECK_FALSE(parse_cli({"reconstruct", "--front", "f.png", "--side", "s.png", "--out", "o.glb"}).has_value());
  CHECK_FALSE(parse_cli({"reconstruct", "--frnt", "f.png"}).has_value());
  CHECK_FALSE(parse_cli({"explode"}).has_value());
  CHECK_FALSE(parse_cli({}).has_value());
}

TEST_CASE("cli: validate and info need a project") {
  CHECK(parse_cli({"validate", "--project", "p.p3d", "--report", "r.json"}).has_value());
  CHECK(parse_cli({"info", "--project", "p.p3d"}).has_value());
  CHECK_FALSE(parse_cli({"info"}).has_value());
}

TEST_CASE("cli: exit codes follow the status") {
  CHECK(exit_code_for(ValidationStatus::Pass) == 0);
  CHECK(exit_code_for(ValidationStatus::NeedsReview) == 2);
  CHECK(exit_code_for(ValidationStatus::Fail) == 1);
  CHECK(exit_code_for(ValidationStatus::NotValidated) == 1);
}

TEST_CASE("cli: --version and --help need no command") {
  CHECK(parse_cli({"--version"})->command == "version");
  CHECK(parse_cli({"--help"})->command == "help");
  CHECK(parse_cli({"-h"})->command == "help");
}

TEST_CASE("cli: input normalisation flags") {
  const auto a = parse_cli({"reconstruct", "--front", "f.jpg", "--side", "s.png", "--top", "t.bmp", "--out", "o.glb",
                            "--normalize", "off", "--target-size", "96", "--bg-tolerance", "30", "--axis-tolerance", "0.2"});
  REQUIRE(a.has_value());
  CHECK_FALSE(a->normalize.enabled);
  CHECK(a->normalize.target_size == 96);
  CHECK(a->normalize.bg_tolerance == 30);
  CHECK(a->normalize.axis_tolerance == doctest::Approx(0.2));
  const auto d = parse_cli({"reconstruct", "--front", "f.png", "--side", "s.png", "--top", "t.png", "--out", "o.glb"});
  REQUIRE(d.has_value());
  CHECK(d->normalize == NormalizeSettings{});
  for (std::vector<std::string> bad : {std::vector<std::string>{"--normalize", "maybe"}, {"--target-size", "8"},
                                       {"--target-size", "600"}, {"--bg-tolerance", "-1"}, {"--axis-tolerance", "3"}}) {
    std::vector<std::string> args{"reconstruct", "--front", "f.png", "--side", "s.png", "--top", "t.png", "--out", "o.glb"};
    args.insert(args.end(), bad.begin(), bad.end());
    const auto r = parse_cli(args);
    CHECK_FALSE(r.has_value());
  }
}

TEST_CASE("cli: lowpoly parses positional drawings and every low-poly option") {
  const auto a = parse_cli({"lowpoly", "f.png", "s.png", "--out", "c.glb", "--budget", "1500", "--joint-loops", "3",
                            "--style", "cel", "--tones", "2", "--template", "cloaked_character", "--baked-cel", "--hull",
                            "--depth", "20", "--accept-inferred-depth", "--accept-status", "NEEDS_REVIEW",
                            "--engine-files", "--report", "r.json", "--renders", "rd", "--project", "p.p3d"});
  REQUIRE(a.has_value());
  CHECK(a->command == "lowpoly");
  CHECK(a->views.at(ViewRole::Front) == "f.png");
  CHECK(a->views.at(ViewRole::Side) == "s.png");
  CHECK(a->lowpoly.tri_budget == 1500);
  CHECK(a->lowpoly.joint_loops == 3);
  CHECK(a->lowpoly.style == lowpoly::Style::Cel);
  CHECK(a->lowpoly.tones == 2);
  CHECK(a->lowpoly.templ == lowpoly::Template::CloakedCharacter);
  CHECK(a->lowpoly.baked_cel == true);
  CHECK(a->lowpoly.hull == true);
  CHECK(a->lowpoly.depth == 20);
  CHECK(a->lowpoly.accept_inferred_depth == true);
  CHECK(a->accept_needs_review);
  CHECK(a->engine_files);
  CHECK(a->style == StylePreset::Crisp);  // the polished-style flag is not touched
  CHECK_FALSE(a->layer_parts.has_value());
  const auto all = parse_cli({"lowpoly", "f.aseprite", "--out", "c.glb", "--layer-parts", "all"});
  REQUIRE(all.has_value());
  REQUIRE(all->layer_parts.has_value());
  CHECK(all->layer_parts->empty());
  const auto some = parse_cli({"lowpoly", "f.psd", "--out", "c.glb", "--layer-parts", "head,body"});
  REQUIRE(some.has_value());
  CHECK(*some->layer_parts == std::vector<std::string>{"head", "body"});
  CHECK_FALSE(parse_cli({"lowpoly", "f.psd", "--out", "c.glb", "--layer-parts", "head,,body"}).has_value());
  CHECK_FALSE(parse_cli({"reconstruct", "--front", "f.psd", "--single-view", "thin", "--out", "c.glb", "--layer-parts", "all"}).has_value());
}

TEST_CASE("cli: lowpoly refuses bad budgets, styles, views and outputs") {
  CHECK_FALSE(parse_cli({"lowpoly", "--front", "f.png", "--out", "c.glb", "--budget", "100"}));
  CHECK_FALSE(parse_cli({"lowpoly", "--front", "f.png", "--out", "c.glb", "--budget", "10x"}));
  CHECK_FALSE(parse_cli({"lowpoly", "--front", "f.png", "--out", "c.glb", "--style", "soft"}));
  CHECK_FALSE(parse_cli({"lowpoly", "--front", "f.png", "--out", "c.obj"}));
  CHECK_FALSE(parse_cli({"lowpoly", "--side", "s.png", "--out", "c.glb"}));
  CHECK_FALSE(parse_cli({"lowpoly", "--front", "f.png", "--back", "b.png", "--out", "c.glb"}));
  CHECK_FALSE(parse_cli({"lowpoly", "--front", "f.png", "--out", "c.glb", "--accept-status", "FAIL"}));
  CHECK_FALSE(parse_cli({"lowpoly", "a.png", "b.png", "c.png", "--out", "c.glb"}));
  const auto ok = parse_cli({"lowpoly", "--front", "f.png", "--top", "t.png", "--out", "c.glb"});
  REQUIRE(ok);
  CHECK(ok->views.size() == 2);
}

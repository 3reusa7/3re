#include "cli/cli_args.hpp"

#include <cctype>
#include <cstdlib>

namespace pixel3d {
namespace {

Unexpected usage(const std::string& why) { return make_error("cli.usage", why); }

}  // namespace

std::string cli_usage() {
  return R"(3re command line

  pixel3d-cli reconstruct --front F.png --side S.png --top T.png
              [--back B.png] [--right R.png] [--bottom U.png]
              [--mode true-solid|voxel] [--profile strict|hand] [--pixels-per-meter N]
              --out model.glb|.gltf|.obj [--preset generic|godot|unity|roblox]
              [--units metres|centimetres|studs|pixels] [--front +z|-z] [--unlit]
              [--report report.json] [--renders DIR] [--turntable] [--project model.p3d]
              [--normalize on|off] [--target-size N] [--bg-tolerance N] [--axis-tolerance F]
              [--style crisp|soft|lowpoly|clay]
  pixel3d-cli reconstruct --front F.png --single-view flat|thin|medium|thick|<depth px> --out ... [same options]
  pixel3d-cli lowpoly --front F.png [--side S.png] [--top T.png] --out character.glb
              [--budget 500-5000] [--joint-loops 1|2|3] [--style flat|cel|outline|cel_outline] [--tones 2|3]
              [--template auto|cloaked_character|humanoid|generic_chain] [--baked-cel] [--hull]
              [--depth N] [--accept-inferred-depth] [--accept-status NEEDS_REVIEW] [--engine-files]
              [--parts project.p3d | --layer-parts all|name,name] [--project out.p3d] [--report report.json]
              [--renders DIR]
              [--pixels-per-meter N] [--normalize on|off]
  pixel3d-cli lowpoly F.png [S.png] --out character.glb [same options]
  pixel3d-cli validate --project model.p3d [--report report.json] [--profile strict|hand]
  pixel3d-cli info --project model.p3d
  pixel3d-cli --version | --help

The model is always written; its status is the exit code: 0 PASS, 2 NEEDS_REVIEW, 1 FAIL or error.
Presets: generic/godot/unity = metres, front +Z; roblox = studs (1 stud = 0.28 m), front -Z.
Views may be PNG, JPEG, BMP, GIF (first frame), TGA, WebP (first frame), Aseprite (.aseprite/.ase, first frame
flattened) or PSD (the stored composite). Real-world images (opaque background,
anti-aliasing, JPEG noise, oversize art) are normalised and every change is printed; clean
pixel art is never changed. --normalize off uses the images exactly as given.
Styles: crisp (default) is the pixel-exact model; soft, lowpoly and clay round the volumes inside
the drawings (every drawn pixel stays covered) and add smooth shading; the polished model is the one
validated and exported. --single-view builds from one drawing (any one view); its depth is assumed,
so the model stays NEEDS_REVIEW unless you give the depth (a preset or a number of pixels).
lowpoly builds a rigged low-poly character (Low-Poly Studio) from Front and optional Side/Top: typed parts
are suggested from the drawing (or taken from --parts, or from the layers of .aseprite/.psd drawings with
--layer-parts: all visible layers, or the named ones), the triangle budget is a hard cap, and the GLB is
written only when its gate is PASS, or NEEDS_REVIEW accepted (--accept-status NEEDS_REVIEW, or front-only
--accept-inferred-depth); a FAIL is never written. Exit code as above.
)";
}

int exit_code_for(ValidationStatus s) {
  switch (s) {
    case ValidationStatus::Pass: return 0;
    case ValidationStatus::NeedsReview: return 2;
    default: return 1;
  }
}

Expected<CliArgs> parse_cli(const std::vector<std::string>& args) {
  if (args.empty()) return usage("missing command");
  CliArgs a;
  a.command = args[0];
  if (a.command == "--version" || a.command == "--help" || a.command == "-h" || a.command == "help") {
    a.command = a.command == "--version" ? "version" : "help";
    return a;
  }
  if (a.command != "reconstruct" && a.command != "validate" && a.command != "info" && a.command != "lowpoly")
    return usage("unknown command '" + a.command + "'");
  const std::map<std::string, ViewRole> view_flags = {{"--front", ViewRole::Front}, {"--side", ViewRole::Side},
                                                      {"--top", ViewRole::Top},     {"--back", ViewRole::Back},
                                                      {"--right", ViewRole::Right}, {"--bottom", ViewRole::Bottom}};
  for (size_t i = 1; i < args.size(); ++i) {
    const std::string& f = args[i];
    if (f == "--turntable") {
      a.turntable = true;
      continue;
    }
    if (f == "--unlit") {
      a.unlit = true;
      continue;
    }
    if (a.command == "lowpoly") {
      if (f == "--baked-cel" || f == "--hull" || f == "--accept-inferred-depth" || f == "--engine-files") {
        if (f == "--baked-cel") a.lowpoly.baked_cel = true;
        else if (f == "--hull") a.lowpoly.hull = true;
        else if (f == "--accept-inferred-depth") a.lowpoly.accept_inferred_depth = true;
        else a.engine_files = true;
        continue;
      }
      if (!f.empty() && f[0] != '-') {  // positional drawings: Front, then Side
        if (!a.views.contains(ViewRole::Front)) a.views[ViewRole::Front] = f;
        else if (!a.views.contains(ViewRole::Side)) a.views[ViewRole::Side] = f;
        else return usage("lowpoly takes at most two positional drawings (Front, Side); use --top for Top");
        continue;
      }
    }
    if (i + 1 >= args.size()) return usage("missing value for " + f);
    const std::string& v = args[++i];
    if (auto it = view_flags.find(f); it != view_flags.end()) a.views[it->second] = v;
    else if (f == "--mode") {
      if (v == "true-solid") a.mode = ReconstructMode::TrueSolid;
      else if (v == "voxel") a.mode = ReconstructMode::Voxel;
      else return usage("--mode must be true-solid or voxel");
    } else if (f == "--profile") {
      if (v == "strict") a.profile = ToleranceProfile::Strict;
      else if (v == "hand") a.profile = ToleranceProfile::Hand;
      else return usage("--profile must be strict or hand");
    } else if (f == "--pixels-per-meter") {
      a.pixels_per_meter = std::atof(v.c_str());
      if (a.pixels_per_meter <= 0) return usage("--pixels-per-meter must be positive");
    } else if (f == "--preset") {
      const auto p = engine_preset_from_string(v);
      if (!p) return usage("--preset must be generic, godot, unity or roblox");
      a.preset = *p;
    } else if (f == "--units") {
      a.units = export_units_from_string(v);
      if (!a.units) return usage("--units must be metres, centimetres, studs or pixels");
    } else if (f == "--front") {
      if (v != "+z" && v != "-z") return usage("--front must be +z or -z");
      a.front_negative_z = v == "-z";
    } else if (f == "--normalize") {
      if (v != "on" && v != "off") return usage("--normalize must be on or off");
      a.normalize.enabled = v == "on";
    } else if (f == "--target-size") {
      a.normalize.target_size = std::atoi(v.c_str());
      if (a.normalize.target_size < 16 || a.normalize.target_size > 512) return usage("--target-size must be 16-512");
    } else if (f == "--bg-tolerance") {
      a.normalize.bg_tolerance = std::atoi(v.c_str());
      if (a.normalize.bg_tolerance < 0 || a.normalize.bg_tolerance > 255 || v.empty() || v[0] == '-')
        return usage("--bg-tolerance must be 0-255");
    } else if (f == "--axis-tolerance") {
      a.normalize.axis_tolerance = std::atof(v.c_str());
      if (!(a.normalize.axis_tolerance >= 0 && a.normalize.axis_tolerance <= 1)) return usage("--axis-tolerance must be 0-1");
    } else if (a.command == "lowpoly" && f == "--style") {
      const auto st = lowpoly::style_from_string(v);
      if (!st) return usage("lowpoly --style must be flat, cel, outline or cel_outline");
      a.lowpoly.style = *st;
    } else if (a.command == "lowpoly" && (f == "--budget" || f == "--joint-loops" || f == "--tones")) {
      char* end = nullptr;
      const long n = std::strtol(v.c_str(), &end, 10);
      if (v.empty() || *end != '\0') return usage(f + " must be a whole number");
      if (f == "--budget") {
        if (n < lowpoly::kMinBudget || n > lowpoly::kMaxBudget) return usage("--budget must be 500-5000 triangles");
        a.lowpoly.tri_budget = static_cast<int>(n);
      } else if (f == "--joint-loops") {
        if (n < 1 || n > 3) return usage("--joint-loops must be 1, 2 or 3");
        a.lowpoly.joint_loops = static_cast<int>(n);
      } else {
        if (n < 2 || n > 3) return usage("--tones must be 2 or 3");
        a.lowpoly.tones = static_cast<int>(n);
      }
    } else if (a.command == "lowpoly" && f == "--template") {
      const auto t = lowpoly::template_from_string(v);
      if (!t) return usage("--template must be auto, cloaked_character, humanoid or generic_chain");
      a.lowpoly.templ = *t;
    } else if (a.command == "lowpoly" && f == "--depth") {
      char* end = nullptr;
      const double depth = std::strtod(v.c_str(), &end);
      if (v.empty() || *end != '\0' || !(depth >= 0 && depth <= 4096)) return usage("--depth must be 0-4096 pixels");
      a.lowpoly.depth = depth;
    } else if (a.command == "lowpoly" && f == "--accept-status") {
      if (v != "NEEDS_REVIEW" && v != "PASS") return usage("--accept-status must be PASS or NEEDS_REVIEW (FAIL is never written)");
      a.accept_needs_review = v == "NEEDS_REVIEW";
    } else if (a.command == "lowpoly" && f == "--parts") {
      a.parts_project = v;
    } else if (a.command == "lowpoly" && f == "--layer-parts") {
      std::vector<std::string> names;
      if (v != "all") {
        size_t start = 0;
        while (start <= v.size()) {
          const size_t comma = v.find(',', start);
          const std::string n = v.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
          if (n.empty()) return usage("--layer-parts takes all or comma-separated layer names");
          names.push_back(n);
          if (comma == std::string::npos) break;
          start = comma + 1;
        }
      }
      a.layer_parts = names;
    } else if (f == "--style") {
      const auto st = style_preset_from_string(v);
      if (!st) return usage("--style must be crisp, soft, lowpoly or clay");
      a.style = *st;
    } else if (f == "--single-view") {
      if (v == "flat" || v == "thin" || v == "medium" || v == "thick") {
        a.single_view_preset = v;
      } else {
        char* end = nullptr;
        const double depth = std::strtod(v.c_str(), &end);
        if (v.empty() || *end != '\0' || !(depth >= 0.25 && depth <= 4096))
          return usage("--single-view must be flat, thin, medium, thick or a depth in pixels (0.25-4096)");
        a.single_view_depth = depth;
      }
    } else if (f == "--project") a.project = v;
    else if (f == "--out") a.out = v;
    else if (f == "--report") a.report = v;
    else if (f == "--renders") a.renders = v;
    else return usage("unknown option " + f);
  }
  if (a.command == "reconstruct") {
    if (a.single_view_preset || a.single_view_depth) {
      if (a.views.size() != 1) return usage("--single-view takes exactly one drawing (e.g. --front F.png)");
    } else {
      for (ViewRole r : kRequiredViewRoles)
        if (!a.views.contains(r)) return usage("reconstruct needs --" + std::string(to_string(r)));
    }
    if (a.out.empty()) return usage("reconstruct needs --out model.glb (or .gltf, .obj)");
    std::string ext = a.out.substr(a.out.find_last_of('.') == std::string::npos ? a.out.size() : a.out.find_last_of('.'));
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext != ".glb" && ext != ".gltf" && ext != ".obj") return usage("--out must end in .glb, .gltf or .obj");
  } else if (a.command == "lowpoly") {
    if (!a.views.contains(ViewRole::Front) && a.parts_project.empty())
      return usage("lowpoly needs a Front drawing (--front F.png)");
    for (const auto& [r, p] : a.views)
      if (r != ViewRole::Front && r != ViewRole::Side && r != ViewRole::Top)
        return usage("lowpoly takes Front with optional Side and Top");
    if (a.out.empty()) return usage("lowpoly needs --out character.glb");
    std::string ext = a.out.substr(a.out.find_last_of('.') == std::string::npos ? a.out.size() : a.out.find_last_of('.'));
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext != ".glb") return usage("lowpoly --out must end in .glb");
  } else if (a.project.empty()) {
    return usage(a.command + " needs --project file.p3d");
  }
  return a;
}

}  // namespace pixel3d

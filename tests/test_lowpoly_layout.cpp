#include <doctest/doctest.h>

#include <algorithm>

#include "lowpoly_helpers.hpp"
#include "pixel3d/lowpoly/layout.hpp"

using namespace pixel3d;
using namespace pixel3d::lowpoly;
using namespace p3dtest;

namespace {

const TubeSpec* tube_with(const Layout& l, int part) {
  for (const auto& t : l.tubes)
    if (std::find(t.parts.begin(), t.parts.end(), part) != t.parts.end()) return &t;
  return nullptr;
}

bool has_message(const Layout& l, const std::string& code) {
  return std::any_of(l.messages.begin(), l.messages.end(), [&](const Message& m) { return m.code == code; });
}

}  // namespace

TEST_CASE("lowpoly layout: the cloaked character fits the cloaked template") {
  const Input in = load_lowpoly_fixture("cloaked");
  const auto l = build_layout(in);
  REQUIRE(l);
  CHECK(l->templ == Template::CloakedCharacter);
  REQUIRE(l->tubes.size() == 5);  // trunk, two legs, two horns
  const TubeSpec* trunk = tube_with(*l, 1000);
  REQUIRE(trunk);
  CHECK(trunk->trunk);
  CHECK(trunk->parts == std::vector<int>{1001, 1000});  // cloak body → head, one loft
  REQUIRE(trunk->joints.size() == 1);                     // the neck
  CHECK(trunk->depth_source == "side");
  for (int leg : {1004, 1005}) {
    const TubeSpec* t = tube_with(*l, leg);
    REQUIRE(t);
    CHECK(t->chain);
    CHECK(t->branch);
    CHECK(t->parent_tube == 0);
    CHECK(t->segments.size() == 2);  // thigh + shin bones from one part
    CHECK(t->extension > 0);
    // hip (attachment) + knee (inferred)
    REQUIRE(t->joints.size() == 2);
    CHECK(t->joints[0].attachment);
    CHECK(t->joints[1].inferred);
  }
  for (int horn : {1002, 1003}) {
    const TubeSpec* t = tube_with(*l, horn);
    REQUIRE(t);
    CHECK(t->branch);
    CHECK_FALSE(t->chain);
    CHECK(t->rigid_bone == l->skeleton.find("Head"));
  }
  CHECK(has_message(*l, "lowpoly.inferred_joint"));
  for (const char* b : {"Hips", "Spine", "Head", "LeftUpperLeg", "LeftLowerLeg", "RightUpperLeg", "RightLowerLeg"})
    CHECK(l->skeleton.find(b) >= 0);
  // The character's left is +X (it faces the Front camera).
  const auto& sk = l->skeleton.bones;
  CHECK(sk[static_cast<size_t>(l->skeleton.find("LeftUpperLeg"))].head.x >
        sk[static_cast<size_t>(l->skeleton.find("RightUpperLeg"))].head.x);
  // Bones hang from their parents.
  for (const Bone& b : sk)
    if (b.parent >= 0) CHECK(b.parent < static_cast<int>(sk.size()));
}

TEST_CASE("lowpoly layout: the humanoid fits the humanoid template with two-part chains") {
  const Input in = load_lowpoly_fixture("humanoid");
  const auto l = build_layout(in);
  REQUIRE(l);
  CHECK(l->templ == Template::Humanoid);
  CHECK(l->tubes.size() == 5);  // trunk, two arms, two legs
  const TubeSpec* arm = tube_with(*l, 1002);
  REQUIRE(arm);
  CHECK(arm->parts == std::vector<int>{1002, 1003});
  CHECK(arm->segments.size() == 2);
  REQUIRE(arm->joints.size() == 2);  // shoulder + elbow
  CHECK_FALSE(arm->joints[1].inferred);
  CHECK(l->skeleton.find("LeftUpperArm") >= 0);
  CHECK(l->skeleton.find("LeftLowerArm") >= 0);
  const Bone& ua = l->skeleton.bones[static_cast<size_t>(l->skeleton.find("LeftUpperArm"))];
  CHECK(ua.head.x > in.dims.W / 2.0);  // left = +X
  CHECK(l->skeleton.bones[static_cast<size_t>(ua.parent)].name == "Spine");
}

TEST_CASE("lowpoly layout: front only infers depth; a missing side mask uses the symmetric partner") {
  const Input in = load_lowpoly_fixture("cloaked", false, 24);
  const auto l = build_layout(in);
  REQUIRE(l);
  for (const auto& t : l->tubes) CHECK((t.depth_source == "inflation" || t.depth_source == "mirror:inflation"));
  for (const auto& t : l->tubes)
    for (const auto& s : t.dense) {
      CHECK(s.half_d <= 12 + 1e-9);
      CHECK(s.z_c == doctest::Approx(12));
    }

  const Input both = load_lowpoly_fixture("cloaked");
  const auto l2 = build_layout(both);
  REQUIRE(l2);
  // The Side view (from −X) shows the right horn and leg; the left ones (hidden) are their mirror images.
  CHECK(tube_with(*l2, 1002)->depth_source.rfind("mirror:side", 0) == 0);
  CHECK(tube_with(*l2, 1003)->depth_source.rfind("side", 0) == 0);
  CHECK(has_message(*l2, "lowpoly.mirrored"));
}

TEST_CASE("lowpoly layout: unfitting parts fall back to a generic chain") {
  Input in = load_lowpoly_fixture("humanoid");
  // Keep the torso, the head and one arm.
  std::erase_if(in.parts, [](const Part& p) { return p.id != 1000 && p.id != 1001 && p.id != 1002 && p.id != 1003; });
  const auto l = build_layout(in);
  REQUIRE(l);
  CHECK(l->templ == Template::GenericChain);
  CHECK(l->tubes.size() == 2);
  // Forcing a template that does not fit is reported.
  in.settings.templ = Template::Humanoid;
  const auto l2 = build_layout(in);
  REQUIRE(l2);
  CHECK(l2->templ == Template::GenericChain);
  CHECK(has_message(*l2, "lowpoly.template_mismatch"));
}

TEST_CASE("lowpoly layout: every station is owned by a part of its tube and bones cover every s") {
  for (const char* variant : {"cloaked", "humanoid"}) {
    CAPTURE(variant);
    const auto l = build_layout(load_lowpoly_fixture(variant));
    REQUIRE(l);
    for (const auto& t : l->tubes) {
      CAPTURE(t.name);
      for (const auto& s : t.dense) {
        if (s.s < 0) continue;  // extension inside the parent
        CHECK(std::find(t.parts.begin(), t.parts.end(), s.owner) != t.parts.end());
        CHECK(bone_at(*l, t, s.s) >= 0);
      }
      CHECK(bone_at(*l, t, -0.25 - 1e-9 * 0) >= 0);
    }
  }
}

TEST_CASE("lowpoly layout: no parts is an error; an external subject mask is used and reported") {
  Input in = load_lowpoly_fixture("cloaked");
  Input none = in;
  none.parts.clear();
  CHECK_FALSE(build_layout(none));
  Mask2D sub = opaque_mask(*in.views[0].aligned);
  in.subject_masks[ViewRole::Front] = SubjectMask{sub, "external:test"};
  const auto l = build_layout(in);
  REQUIRE(l);
  CHECK(l->subject.at(ViewRole::Front) == sub);
}

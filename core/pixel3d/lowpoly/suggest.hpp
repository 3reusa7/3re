#pragma once
// Auto-split suggestion: typed parts proposed from the Front drawing alone, for
// the user to confirm or edit (never applied silently).
//
// 1. Colour regions (outline colour separates them); small regions enclosed by
//    one region (eyes, buttons) are texture, not parts, and merge into it; ink
//    pixels join their nearest region so the masks cover the whole drawing.
// 2. Every region is split by shape, which also works for drawings without ink
//    lines or colour changes between parts: erosion at half the region's largest
//    distance-transform value leaves its cores (head, body); what the opening
//    removes are protrusions (legs, arms, horns); the rest of the region goes to
//    the nearest core.
// 3. Types from layout and shape: the topmost large round part is the head, the
//    largest other part the body (cloth when it widens downward), elongated parts
//    on the head are accessories (horns), elongated parts on the body or on limbs
//    are limbs, anything else an accessory.
#include <optional>
#include <vector>

#include "pixel3d/image/palette.hpp"
#include "pixel3d/lowpoly/types.hpp"

namespace pixel3d::lowpoly {

struct Suggestion {
  std::vector<Part> parts;  // ids from kFirstSuggestedPart, Front masks (Side masks when a Side view is given)
  Template templ = Template::GenericChain;  // the template these parts would fit
  std::vector<Message> messages;
};

inline constexpr int kFirstSuggestedPart = 1000;

// `subject`: optional external subject mask (Smart Pipeline hook a); default alpha.
// `side`: optional aligned Side view: each part gets the Side pixels of its own
// colour family in its rows (reported as derived).
Suggestion suggest_parts(const SourceView& front, const MaterialFamilies& fam, uint8_t alpha_threshold,
                         const Mask2D* subject = nullptr, const SourceView* side = nullptr);

}  // namespace pixel3d::lowpoly

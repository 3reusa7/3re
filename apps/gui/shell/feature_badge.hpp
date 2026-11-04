#pragma once
// Honest feature labels: Experimental (works, results may need review) and Coming Soon (not available).
#include <QLabel>

namespace p3dgui {
enum class Feature { Experimental, ComingSoon };
QLabel* make_feature_badge(Feature f, QWidget* parent = nullptr);
}  // namespace p3dgui

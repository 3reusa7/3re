#pragma once
// Brain evaluation runner (docs/agent/chat-eval/README.md).
#include "chat.hpp"

namespace p3dchat {
// Sets 1–3 for one brain; set2_dir holds the renamed pictures (<set>/<IMG_n.png>), "" skips set 2.
int run_brain_eval(Provider& brain, const fs::path& eval_dir, const fs::path& out, const fs::path& set2_dir);
}  // namespace p3dchat

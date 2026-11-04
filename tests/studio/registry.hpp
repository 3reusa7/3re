#pragma once
// One Qt Test executable for the 3re studio: every test class registers itself
// and main() runs them all (QTest::qExec per class, non-zero exit if any fails).
#include <QObject>
#include <functional>
#include <memory>
#include <vector>

namespace threere_test {
using Factory = std::function<std::unique_ptr<QObject>()>;
inline std::vector<Factory>& registry() {
  static std::vector<Factory> r;
  return r;
}
struct Register {
  explicit Register(Factory f) { registry().push_back(std::move(f)); }
};
}  // namespace threere_test

#define THREERE_TEST(Class) \
  static const threere_test::Register threere_register_##Class([] { return std::unique_ptr<QObject>(new Class); });

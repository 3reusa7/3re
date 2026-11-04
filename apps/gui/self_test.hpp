#pragma once
// `3re-studio --self-test`: checks that a release folder is complete and self-contained
// (config with relative paths only, runtime, pinned checksums, models, segmentation,
// helpers, CUDA probe). Used by tools/package.ps1 and tools/verify_portable.ps1.
#include <QByteArray>
#include <QList>
#include <QString>

namespace p3dgui {

struct SelfTestItem {
  QString name;
  bool ok = false;
  QString detail;
  bool fatal = true;  // false: reported, but does not fail the release (e.g. no GPU on this machine)
};

QList<SelfTestItem> run_self_test(const QString& appDir, bool deep);
bool self_test_passed(const QList<SelfTestItem>& items);
QByteArray self_test_json(const QList<SelfTestItem>& items);

}  // namespace p3dgui

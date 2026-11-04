#include "studio/angles_panel.hpp"

#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "backends/image_input.hpp"
#include "icons.hpp"
#include "widgets.hpp"

namespace p3dgui {
namespace {
struct Choice { const char* label; const char* text; };
const Choice kChoices[] = {{"auto", QT_TRANSLATE_NOOP("AnglesPanel", "Find the angle")},
                           {"back", QT_TRANSLATE_NOOP("AnglesPanel", "Back")},
                           {"left", QT_TRANSLATE_NOOP("AnglesPanel", "Left side (its left)")},
                           {"right", QT_TRANSLATE_NOOP("AnglesPanel", "Right side (its right)")},
                           {"front_left", QT_TRANSLATE_NOOP("AnglesPanel", "Front three-quarter, left")},
                           {"front_right", QT_TRANSLATE_NOOP("AnglesPanel", "Front three-quarter, right")},
                           {"back_left", QT_TRANSLATE_NOOP("AnglesPanel", "Back three-quarter, left")},
                           {"back_right", QT_TRANSLATE_NOOP("AnglesPanel", "Back three-quarter, right")},
                           {"custom", QT_TRANSLATE_NOOP("AnglesPanel", "Custom angle")}};
}  // namespace

AnglesPanel::AnglesPanel(QWidget* parent) : QWidget(parent) {
  setObjectName("image3dAngles");
  auto* col = new QVBoxLayout(this);
  col->setContentsMargins(0, 0, 0, 0);
  col->setSpacing(theme::space::sm);
  hint_ = make_label(tr("Optional: up to five more pictures of the same subject (back, sides, three-quarter views; a character "
                        "turnaround sheet works too). The 3D shape still comes from the front picture; these pictures paint "
                        "the sides it cannot see. Each one is matched to the model and rejected if it does not fit."),
                     "secondary", this);
  hint_->setWordWrap(true);
  col->addWidget(hint_);
  auto* buttons = new QHBoxLayout;
  add_ = make_button(tr("Add Pictures…"), "ghost", icon(Icon::Import), this);
  add_->setObjectName("image3dAnglesAdd");
  split_ = make_button(tr("Split a Turnaround Sheet…"), "ghost", {}, this);
  split_->setObjectName("image3dAnglesSplit");
  split_->setToolTip(tr("One picture with several views side by side (front, side, back, ...): split into separate pictures."));
  buttons->addWidget(add_);
  buttons->addWidget(split_);
  buttons->addStretch(1);
  col->addLayout(buttons);
  rows_ = new QVBoxLayout;
  rows_->setSpacing(theme::space::xs);
  col->addLayout(rows_);
  connect(add_, &QPushButton::clicked, this, [this] {
    const auto files = QFileDialog::getOpenFileNames(this, tr("Pictures from other angles"), {}, tr("Images (*.png *.jpg *.jpeg *.webp *.bmp)"));
    if (!files.isEmpty()) addFiles(files);
  });
  connect(split_, &QPushButton::clicked, this, [this] {
    const auto sheet = QFileDialog::getOpenFileName(this, tr("Turnaround sheet"), {}, tr("Images (*.png *.jpg *.jpeg *.webp *.bmp)"));
    if (sheet.isEmpty()) return;
    QString error;
    const QString out = QDir(QFileInfo(sheet).absolutePath()).filePath(QFileInfo(sheet).completeBaseName() + " - split");
    const auto parts = splitSheet(sheet, out, &error);
    if (parts.size() < 2) {
      hint_->setText(error.isEmpty() ? tr("No separate figures were found in that picture.") : error);
      return;
    }
    emit sheetSplit(parts.front());
    const int before = int(items_.size());
    addFiles(parts.mid(1));
    const auto labels = defaultLabels(int(parts.size()));
    for (int k = 0; k + before < items_.size() && k < labels.size(); ++k) items_[before + k].label = labels[k];
    rebuild();
    emit changed();
    hint_->setText(tr("Split into %1 pictures (written next to the sheet). The first is the front; check each other picture's angle.").arg(parts.size()));
  });
  rebuild();
}

QString AnglesPanel::displayName(const QString& label) {
  const QString base = label.section(':', 0, 0);
  for (const auto& c : kChoices)
    if (base == c.label) return base == "custom" ? tr("%1 degrees from the front").arg(label.section(':', 1)) : tr(c.text);
  return label;
}

QStringList AnglesPanel::defaultLabels(int figures) {
  // A turnaround sheet usually reads front, side, back, other side; which side is which differs between
  // artists, so the sides are found from the outline ("auto"); the middle figure after the front is the back.
  QStringList out;
  const int extras = figures - 1, back = figures == 2 ? 0 : (figures - 1) / 2;
  for (int k = 0; k < extras; ++k) out << (k == back ? "back" : "auto");
  return out;
}

void AnglesPanel::setItems(const QVector<Item>& items) {
  items_ = items.mid(0, kMaxPictures);
  rebuild();
}

void AnglesPanel::clear() {
  items_.clear();
  rebuild();
}

void AnglesPanel::setLabel(int index, const QString& label) {
  if (index < 0 || index >= items_.size()) return;
  items_[index].label = label;
  items_[index].status.clear();
  rebuild();
  emit changed();
}

int AnglesPanel::addFiles(const QStringList& files) {
  int added = 0;
  QStringList problems;
  for (const auto& f : files) {
    if (items_.size() >= kMaxPictures) {
      problems << tr("At most %1 extra pictures.").arg(kMaxPictures);
      break;
    }
    const auto input = p3dasset::prepare_image(f, p3dasset::ImageInputOptions{p3dasset::InputType(input_type_), remove_background_});
    if (!input.error.isEmpty() || input.cutout.isNull()) {
      problems << QFileInfo(f).fileName() + ": " + (input.error.isEmpty() ? tr("could not be read") : input.error);
      continue;
    }
    items_.push_back({f, "auto", input.cutout, {}});
    ++added;
  }
  rebuild();
  if (added) emit changed();
  if (!problems.isEmpty()) hint_->setText(problems.join('\n'));
  return added;
}

QStringList AnglesPanel::splitSheet(const QString& sheet, const QString& out_dir, QString* error) {
  QImage img(sheet);
  if (img.isNull()) {
    if (error) *error = tr("The sheet could not be read.");
    return {};
  }
  img = img.convertToFormat(QImage::Format_ARGB32);
  const int W = img.width(), H = img.height();
  // Foreground: opaque pixels when the sheet has transparency, else pixels far from the border colour.
  bool alpha = false;
  for (int y = 0; y < H && !alpha; y += 4)
    for (int x = 0; x < W; x += 4)
      if (qAlpha(img.pixel(x, y)) < 128) { alpha = true; break; }
  std::vector<int> r, g, b;
  for (int x = 0; x < W; ++x)
    for (int y : {0, H - 1}) { const QRgb p = img.pixel(x, y); r.push_back(qRed(p)); g.push_back(qGreen(p)); b.push_back(qBlue(p)); }
  for (int y = 0; y < H; ++y)
    for (int x : {0, W - 1}) { const QRgb p = img.pixel(x, y); r.push_back(qRed(p)); g.push_back(qGreen(p)); b.push_back(qBlue(p)); }
  auto median = [](std::vector<int>& v) { std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end()); return v[v.size() / 2]; };
  const int br = median(r), bg = median(g), bb = median(b);
  auto fg = [&](int x, int y) {
    const QRgb p = img.pixel(x, y);
    if (alpha) return qAlpha(p) >= 128;
    return std::abs(qRed(p) - br) + std::abs(qGreen(p) - bg) + std::abs(qBlue(p) - bb) > 60;
  };
  std::vector<int> column(size_t(W), 0);
  for (int x = 0; x < W; ++x)
    for (int y = 0; y < H; ++y) column[size_t(x)] += fg(x, y);
  const int empty = std::max(1, H / 500);
  std::vector<std::pair<int, int>> runs;
  for (int x = 0; x < W;) {
    while (x < W && column[size_t(x)] <= empty) ++x;
    const int s = x;
    while (x < W && column[size_t(x)] > empty) ++x;
    if (x > s) runs.push_back({s, x});
  }
  // Merge figures split by a thin gap (a raised arm, a loose strand) and drop specks.
  std::vector<std::pair<int, int>> merged;
  for (const auto& run : runs) {
    if (!merged.empty() && run.first - merged.back().second < std::max(2, W / 60)) merged.back().second = run.second;
    else merged.push_back(run);
  }
  merged.erase(std::remove_if(merged.begin(), merged.end(), [&](const auto& m) { return m.second - m.first < std::max(4, W / 40); }), merged.end());
  if (merged.size() < 2) {
    if (error) *error = tr("Only one figure was found: the sheet needs clear gaps between the views.");
    return {};
  }
  if (merged.size() > size_t(kMaxPictures + 1)) {
    if (error) *error = tr("%1 figures were found; at most %2 pictures are used.").arg(merged.size()).arg(kMaxPictures + 1);
    merged.resize(size_t(kMaxPictures + 1));
  }
  if (!QDir().mkpath(out_dir)) {
    if (error) *error = tr("Could not create %1.").arg(QDir::toNativeSeparators(out_dir));
    return {};
  }
  QStringList out;
  for (size_t k = 0; k < merged.size(); ++k) {
    int y0 = H, y1 = -1;
    for (int y = 0; y < H; ++y)
      for (int x = merged[k].first; x < merged[k].second; ++x)
        if (fg(x, y)) { y0 = std::min(y0, y); y1 = std::max(y1, y); break; }
    const int m = std::max(4, (merged[k].second - merged[k].first) / 20);
    const QRect rect(QPoint(std::max(0, merged[k].first - m), std::max(0, y0 - m)), QPoint(std::min(W - 1, merged[k].second - 1 + m), std::min(H - 1, y1 + m)));
    QString file = QDir(out_dir).filePath(QStringLiteral("%1-%2.png").arg(QFileInfo(sheet).completeBaseName()).arg(k + 1));
    for (int n = 2; QFileInfo::exists(file); ++n)  // never overwrite
      file = QDir(out_dir).filePath(QStringLiteral("%1-%2 (%3).png").arg(QFileInfo(sheet).completeBaseName()).arg(k + 1).arg(n));
    if (!img.copy(rect).save(file, "PNG")) {
      if (error) *error = tr("Could not write %1.").arg(QDir::toNativeSeparators(file));
      return {};
    }
    out << file;
  }
  return out;
}

void AnglesPanel::setResults(const QJsonArray& views, const QJsonObject& ownership) {
  for (auto& it : items_) it.status.clear();
  for (const auto& v : views) {
    const auto o = v.toObject();
    // Picture k is saved as view-k-<label>.png (texture pass) or retexture-view-k.png (Re-texture all).
    static const QRegularExpression index(QStringLiteral("(?:^|-)view-(\\d+)"));
    const auto m = index.match(o.value("name").toString());
    if (!m.hasMatch()) continue;
    const int k = m.captured(1).toInt() - 1;
    if (k < 0 || k >= items_.size()) continue;
    if (o.value("status").toString() == "accepted") {
      const QString label = o.value("resolved_label").toString(o.value("label").toString());
      QString s = tr("Used · outline fit %1% · %2 degrees from the front").arg(qRound(o.value("fit").toDouble() * 100)).arg(qRound(o.value("yaw_from_front").toDouble()));
      if (ownership.contains(label)) s += tr(" · paints %1% of the surface").arg(qRound(ownership.value(label).toDouble() * 100));
      items_[k].status = s;
    } else {
      items_[k].status = tr("Not used: %1").arg(o.value("reason").toString());
    }
  }
  rebuild();
}

void AnglesPanel::rebuild() {
  while (auto* item = rows_->takeAt(0)) {
    if (auto* w = item->widget()) w->deleteLater();
    delete item;
  }
  for (int k = 0; k < items_.size(); ++k) {
    auto* row = new QWidget(this);
    row->setObjectName(QStringLiteral("image3dAngle%1").arg(k));
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    auto* thumb = new QLabel(row);
    thumb->setFixedSize(56, 56);
    thumb->setAlignment(Qt::AlignCenter);
    thumb->setProperty("checker", true);
    thumb->setPixmap(QPixmap::fromImage(items_[k].cutout).scaled(54, 54, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    h->addWidget(thumb);
    auto* mid = new QVBoxLayout;
    auto* name = make_label(QFileInfo(items_[k].file).fileName(), nullptr, row);
    name->setToolTip(QDir::toNativeSeparators(items_[k].file));
    mid->addWidget(name);
    auto* pick = new QHBoxLayout;
    auto* combo = new QComboBox(row);
    combo->setObjectName(QStringLiteral("image3dAngleLabel%1").arg(k));
    for (const auto& c : kChoices) combo->addItem(tr(c.text), QString::fromLatin1(c.label));
    const QString base = items_[k].label.section(':', 0, 0);
    combo->setCurrentIndex(std::max(0, combo->findData(base)));
    auto* deg = new QSpinBox(row);
    deg->setRange(0, 359);
    deg->setSuffix(tr(" °"));
    deg->setToolTip(tr("Degrees from the front, turning towards the subject's left side."));
    deg->setValue(base == "custom" ? items_[k].label.section(':', 1).toInt() : 90);
    deg->setVisible(base == "custom");
    pick->addWidget(combo, 1);
    pick->addWidget(deg);
    mid->addLayout(pick);
    if (!items_[k].status.isEmpty()) {
      auto* st = make_label(items_[k].status, "secondary", row);
      st->setObjectName(QStringLiteral("image3dAngleStatus%1").arg(k));
      st->setWordWrap(true);
      theme::set_prop(st, "tone", items_[k].status.startsWith(tr("Used")) ? "pass" : "review");
      mid->addWidget(st);
    }
    h->addLayout(mid, 1);
    auto* remove = make_button(QString(), "ghost", icon(Icon::Close), row);
    remove->setToolTip(tr("Remove this picture"));
    h->addWidget(remove, 0, Qt::AlignTop);
    rows_->addWidget(row);
    connect(combo, &QComboBox::currentIndexChanged, this, [this, k, combo, deg] {
      const QString l = combo->currentData().toString();
      deg->setVisible(l == "custom");
      QMetaObject::invokeMethod(this, [this, k, l, deg] { setLabel(k, l == "custom" ? QStringLiteral("custom:%1").arg(deg->value()) : l); }, Qt::QueuedConnection);
    });
    connect(deg, &QSpinBox::editingFinished, this, [this, k, deg] { setLabel(k, QStringLiteral("custom:%1").arg(deg->value())); });
    connect(remove, &QPushButton::clicked, this, [this, k] {
      QMetaObject::invokeMethod(this, [this, k] {
        if (k < items_.size()) items_.removeAt(k);
        rebuild();
        emit changed();
      }, Qt::QueuedConnection);
    });
  }
  add_->setEnabled(items_.size() < kMaxPictures);
}

}  // namespace p3dgui

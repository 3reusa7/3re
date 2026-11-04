#include <QApplication>
#include <QDir>
#include <QLocale>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QSettings>
#include <QTest>

#include "registry.hpp"

// THREERE_TEST_LOG_DIR=<dir>: one Qt Test log per class (<dir>/<Class>.txt); otherwise stdout.
int main(int argc, char** argv) {
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
  QStandardPaths::setTestModeEnabled(true);
  QApplication::setOrganizationName("3re-tests");
  QApplication::setApplicationName("3re-tests");
  QTemporaryDir settings;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
  // The Agent page reattaches unfinished conversations from Documents\3re: tests use their own empty folder.
  QTemporaryDir documents;
  qputenv("THREERE_DOCUMENTS", documents.path().toUtf8());
  const QString logs = qEnvironmentVariable("THREERE_TEST_LOG_DIR");
  int failed = 0;
  for (const auto& make : threere_test::registry()) {
    auto test = make();
    QStringList args;
    for (int i = 0; i < argc; ++i) args << QString::fromLocal8Bit(argv[i]);
    if (!logs.isEmpty()) {
      QDir().mkpath(logs);
      args << "-o" << QDir(logs).filePath(QString::fromLatin1(test->metaObject()->className()) + ".txt,txt");
    }
    failed += QTest::qExec(test.get(), args) != 0;
  }
  return failed;
}

#pragma once
#include <QJsonObject>
#include <QString>
namespace p3dasset {
// Pure classifications are shared by the worker and regression tests.
QJsonObject cudaDiagnosis(bool environment_ok, bool registered, int runtime_code,
                         const QString& runtime_name, const QString& runtime_error, int devices);
QJsonObject runCudaHealth(const QString& runtime);
void observeInferenceLine(QJsonObject& report,const QString& line);
void finishInference(QJsonObject& report,bool success,bool outputs);
bool observedGpuSample(bool api_ok,bool dedicated_pid,bool shared_pid);
bool collectGpuValue(const QString& instance,const QString& prefix,bool valid,qint64 value,quint64& sum);
QJsonObject nativeTexturePlan(const QString& model_directory);
}

#pragma once

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <functional>

class QWidget;

namespace updater {
enum class Outcome { UpToDate, UpdateAvailable, Unavailable, Canceled };
enum class Reason { Startup, Periodic, Manual };
using Callback = std::function<void(Outcome, const QString &message)>;

inline constexpr const char *kInstallHeader = "X-Ivynote-Install";

QString reasonParam(Reason reason);
bool isValidInstallId(const QString &id);
QString installIdIn(const QString &dir);
QString installId();
QString buildCheckPath(Reason reason);
QList<QPair<QByteArray, QByteArray>> checkHeaders(bool statsEnabled, const QString &dir = {});

void check(QWidget *window, Reason reason, Callback onFinished = {});
inline void check(QWidget *window, bool manual, Callback onFinished = {}) {
    check(window, manual ? Reason::Manual : Reason::Periodic, std::move(onFinished));
}
}

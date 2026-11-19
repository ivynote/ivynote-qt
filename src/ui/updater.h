#pragma once

#include <QString>
#include <functional>

class QWidget;

namespace updater {
enum class Outcome { UpToDate, UpdateAvailable, Unavailable, Canceled };
using Callback = std::function<void(Outcome, const QString &message)>;
void check(QWidget *window, bool manual, Callback onFinished = {});
}

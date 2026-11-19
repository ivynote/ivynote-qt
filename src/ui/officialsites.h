#pragma once

#include <QJsonObject>
#include <functional>

class QObject;
class QNetworkAccessManager;

namespace official {
using AcceptResponse = std::function<bool(const QJsonObject &)>;
void fetch(QNetworkAccessManager *network, QObject *context, const QString &path,
           AcceptResponse accept, std::function<void()> failure = {});
}

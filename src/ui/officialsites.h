#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <functional>

class QObject;
class QNetworkAccessManager;

namespace official {
using AcceptResponse = std::function<bool(const QJsonObject &)>;
using Headers = QList<QPair<QByteArray, QByteArray>>;
void fetch(QNetworkAccessManager *network, QObject *context, const QString &path,
           AcceptResponse accept, std::function<void()> failure = {},
           Headers headers = {});
}

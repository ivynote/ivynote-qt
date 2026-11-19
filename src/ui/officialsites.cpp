#include "ui/officialsites.h"

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QTimer>

namespace {
const QStringList sites{QStringLiteral("https://ivynote.net"), QStringLiteral("https://ivynote.app")};
constexpr qsizetype responseLimit = 1024 * 1024;

void attempt(QNetworkAccessManager *network, QObject *context, const QString &path,
             const official::AcceptResponse &accept, const std::function<void()> &failure, int index) {
    if (index == sites.size()) {
        if (failure) failure();
        return;
    }
    QNetworkRequest request(QUrl(sites.at(index) + path));
    request.setTransferTimeout(6000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    auto *reply = network->get(request);
    QTimer::singleShot(6000, reply, [reply] {
        if (!reply->isFinished()) reply->abort();
    });
    reply->setReadBufferSize(responseLimit + 1);
    QObject::connect(reply, &QNetworkReply::readyRead, reply, [reply] {
        if (reply->bytesAvailable() > responseLimit) reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    QObject::connect(reply, &QNetworkReply::finished, context, [=, manager = QPointer(network), owner = QPointer(context)] {
        const auto payload = reply->readAll();
        if (payload.size() > responseLimit) {
            if (manager && owner) attempt(manager, owner, path, accept, failure, index + 1);
            return;
        }
        const auto document = QJsonDocument::fromJson(payload);
        if (reply->error() == QNetworkReply::NoError &&
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200 &&
            document.isObject() && accept(document.object())) return;
        if (manager && owner) attempt(manager, owner, path, accept, failure, index + 1);
    });
}
}

void official::fetch(QNetworkAccessManager *network, QObject *context, const QString &path,
                     AcceptResponse accept, std::function<void()> failure) {
    attempt(network, context, path, accept, failure, 0);
}

#include "sync/transport.h"

#include "sync/api.h"

#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtConcurrent/QtConcurrentRun>

namespace {

TransportError errorOf(const ApiReply &r) {
    return r.ok ? TransportError::success() : TransportError::failure(r.status, r.error);
}

} // namespace

void ApiTransport::fetch(const QString &path, std::function<void(TransportError, PullResponse)> done) {
    // Pulls carry every changed body and have no paging (guide 12 #3): give
    // them time, and parse off the UI thread.
    m_api->get(path, m_api, [api = m_api, done](const ApiReply &r) {
        if (!r.ok) {
            done(errorOf(r), {});
            return;
        }
        auto *watcher = new QFutureWatcher<PullResponse>(api);
        QObject::connect(watcher, &QFutureWatcher<PullResponse>::finished, api, [watcher, done] {
            watcher->deleteLater();
            done(TransportError::success(), watcher->result());
        });
        watcher->setFuture(QtConcurrent::run([body = r.body] {
            return PullResponse::fromJson(QJsonDocument::fromJson(body).object());
        }));
    }, 300000);
}

void ApiTransport::snapshot(std::function<void(TransportError, PullResponse)> done) {
    fetch(QStringLiteral("/sync/snapshot"), std::move(done));
}

void ApiTransport::pull(qint64 since, std::function<void(TransportError, PullResponse)> done) {
    fetch(QStringLiteral("/sync/pull?since=%1").arg(since), std::move(done));
}

void ApiTransport::push(const QList<QJsonObject> &changes,
                        std::function<void(TransportError, QList<ChangeResult>, qint64)> done) {
    QJsonArray arr;
    for (const auto &c : changes) arr.append(c);
    const QByteArray body = QJsonDocument(QJsonObject{{"changes", arr}}).toJson(QJsonDocument::Compact);
    m_api->send("POST", QStringLiteral("/sync/push"), body, m_api, [done](const ApiReply &r) {
        if (!r.ok) {
            done(errorOf(r), {}, 0);
            return;
        }
        const QJsonObject o = r.json.toObject();
        QList<ChangeResult> results;
        for (const auto &v : o.value(QLatin1String("results")).toArray()) results << ChangeResult::fromJson(v.toObject());
        done(TransportError::success(), results, qint64(o.value(QLatin1String("seq")).toDouble()));
    }, 300000);
}

void ApiTransport::upload(const QByteArray &data, const QString &name, const QString &mime,
                          std::function<void(TransportError, Attachment)> done) {
    m_api->upload(data, name, mime, m_api, [done](const ApiReply &r) {
        if (!r.ok) {
            done(errorOf(r), {});
            return;
        }
        done(TransportError::success(), Attachment::fromJson(r.json.toObject().value(QLatin1String("attachment")).toObject()));
    });
}

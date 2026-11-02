#include "sync/api.h"

#include <QHostAddress>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

ApiClient::ApiClient(const QUrl &server, QObject *parent)
    : QObject(parent), m_server(server), m_nam(new QNetworkAccessManager(this)) {
    m_nam->setAutoDeleteReplies(false);
}

QNetworkRequest ApiClient::request(const QString &path, int timeoutMs) const {
    QUrl url = m_server;
    QString p = path;
    if (p.startsWith(QLatin1String("//"))) p = p.mid(1);
    else p = QStringLiteral("/api/v1") + p;
    const qsizetype q = p.indexOf(u'?');
    url.setPath(url.path() + (q >= 0 ? p.left(q) : p));
    if (q >= 0) url.setQuery(p.mid(q + 1));
    QNetworkRequest req(url);
    if (!m_token.isEmpty()) req.setRawHeader("Authorization", "Bearer " + m_token.toUtf8());
    if (!m_device.isEmpty()) req.setRawHeader("X-Ivy-Device", m_device.toUtf8());
    req.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    req.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Ivynote-Qt/" IVY_VERSION));
    if (timeoutMs > 0) req.setTransferTimeout(timeoutMs);
    return req;
}

QNetworkReply *ApiClient::send(const QByteArray &method, const QString &path, const QByteArray &body,
                               const QObject *context, ApiCallback cb, int timeoutMs, const QByteArray &contentType) {
    QNetworkRequest req = request(path, timeoutMs);
    if (!body.isEmpty() || method != "GET") req.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
    QNetworkReply *reply = m_nam->sendCustomRequest(req, method, body);
    finish(reply, context, std::move(cb));
    return reply;
}

QNetworkReply *ApiClient::get(const QString &path, const QObject *context, ApiCallback cb, int timeoutMs) {
    QNetworkReply *reply = m_nam->get(request(path, timeoutMs));
    finish(reply, context, std::move(cb));
    return reply;
}

QNetworkReply *ApiClient::postJson(const QString &path, const QJsonValue &body, const QObject *context, ApiCallback cb, int timeoutMs) {
    return send("POST", path, QJsonDocument(body.toObject()).toJson(QJsonDocument::Compact), context, std::move(cb), timeoutMs);
}

QNetworkReply *ApiClient::putJson(const QString &path, const QJsonValue &body, const QObject *context, ApiCallback cb) {
    return send("PUT", path, QJsonDocument(body.toObject()).toJson(QJsonDocument::Compact), context, std::move(cb));
}

QNetworkReply *ApiClient::patchJson(const QString &path, const QJsonValue &body, const QObject *context, ApiCallback cb) {
    return send("PATCH", path, QJsonDocument(body.toObject()).toJson(QJsonDocument::Compact), context, std::move(cb));
}

QNetworkReply *ApiClient::del(const QString &path, const QObject *context, ApiCallback cb) {
    return send("DELETE", path, {}, context, std::move(cb));
}

QNetworkReply *ApiClient::upload(const QByteArray &data, const QString &name, const QString &mime, const QObject *context, ApiCallback cb) {
    auto *multi = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    QHttpPart part;
    QString safe = name;
    safe.replace(u'"', u'\'');
    part.setHeader(QNetworkRequest::ContentDispositionHeader,
                   QStringLiteral("form-data; name=\"file\"; filename=\"%1\"").arg(safe));
    part.setHeader(QNetworkRequest::ContentTypeHeader, mime.isEmpty() ? QStringLiteral("application/octet-stream") : mime);
    part.setBody(data);
    multi->append(part);
    QNetworkRequest req = request(QStringLiteral("/attachments"), 0);
    QNetworkReply *reply = m_nam->post(req, multi);
    multi->setParent(reply);
    finish(reply, context, std::move(cb));
    return reply;
}

void ApiClient::finish(QNetworkReply *reply, const QObject *context, ApiCallback cb) {
    QPointer<const QObject> guard(context);
    connect(reply, &QNetworkReply::finished, this, [reply, guard, context, cb = std::move(cb)] {
        reply->deleteLater();
        if (context && !guard) return;
        ApiReply r;
        r.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        r.body = reply->readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(r.body);
        r.json = doc.isObject() ? QJsonValue(doc.object()) : doc.isArray() ? QJsonValue(doc.array()) : QJsonValue();
        if (reply->error() == QNetworkReply::OperationCanceledError && r.status == 0) {
            r.error = QStringLiteral("canceled");
        } else if (r.status == 0) {
            r.error = reply->errorString();
        } else if (r.status >= 200 && r.status < 300) {
            r.ok = true;
        } else {
            r.error = r.json.toObject().value(QLatin1String("error")).toString();
            if (r.error.isEmpty()) r.error = QStringLiteral("HTTP %1").arg(r.status);
        }
        if (cb) cb(r);
    });
}

QUrl ApiClient::normalizeServer(const QString &input) {
    QString s = input.trimmed();
    if (s.isEmpty()) return {};
    if (!s.contains(QLatin1String("://"))) s.prepend(QStringLiteral("https://"));
    QUrl url(s, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty()) return {};
    if (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https")) return {};
    QString path = url.path();
    while (path.endsWith(u'/')) path.chop(1);
    if (path.endsWith(QLatin1String("/api/v1"))) path.chop(7);
    while (path.endsWith(u'/')) path.chop(1);
    url.setPath(path);
    url.setQuery(QString());
    url.setFragment(QString());
    return url;
}

bool ApiClient::isLocalHost(const QUrl &url) {
    const QString host = url.host().toLower();
    if (host == QLatin1String("localhost") || host.endsWith(QLatin1String(".localhost"))) return true;
    const QHostAddress addr(host);
    return !addr.isNull() && addr.isLoopback();
}

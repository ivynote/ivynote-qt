#include "sync/api.h"

#include "storage/crypto.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QHostAddress>
#include <QHttpMultiPart>
#include <QImage>
#include <QImageReader>
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

namespace {

constexpr qsizetype kDirectThumbMaxInput = 40 << 20;
constexpr int kDirectThumbMaxSide = 480;

struct ImageThumbMeta {
    int width = 0;
    int height = 0;
    QByteArray thumbJpeg;
};

ImageThumbMeta buildImageThumb(const QByteArray &data, const QString &mime) {
    ImageThumbMeta meta;
    if (!mime.startsWith(QLatin1String("image/"), Qt::CaseInsensitive) || data.size() > kDirectThumbMaxInput) return meta;
    QBuffer inBuf;
    inBuf.setData(data);
    if (!inBuf.open(QIODevice::ReadOnly)) return meta;
    QImageReader reader(&inBuf);
    reader.setAutoTransform(true);
    const QImage img = reader.read();
    if (img.isNull() || img.width() <= 0 || img.height() <= 0) return meta;
    meta.width = img.width();
    meta.height = img.height();
    QImage scaled = img;
    if (img.width() > kDirectThumbMaxSide || img.height() > kDirectThumbMaxSide) {
        scaled = img.scaled(kDirectThumbMaxSide, kDirectThumbMaxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    QBuffer outBuf(&meta.thumbJpeg);
    if (outBuf.open(QIODevice::WriteOnly)) {
        if (!scaled.save(&outBuf, "JPEG", 82)) meta.thumbJpeg.clear();
    }
    return meta;
}

bool shouldPropagateIntentError(const ApiReply &r) {
    return r.status == 401 || r.status == 402 || r.status == 403 || r.status == 413 || r.transient();
}

} // namespace

QNetworkReply *ApiClient::getDirect(const QUrl &url, const QObject *context, ApiCallback cb, int timeoutMs) {
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    req.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Ivynote-Qt/" IVY_VERSION));
    if (timeoutMs > 0) req.setTransferTimeout(timeoutMs);
    QNetworkReply *reply = m_nam->get(req);
    finish(reply, context, std::move(cb));
    return reply;
}

QNetworkReply *ApiClient::putDirect(const QUrl &url, const QByteArray &data, const QObject *context, ApiCallback cb, int timeoutMs) {
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    req.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Ivynote-Qt/" IVY_VERSION));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/octet-stream"));
    if (timeoutMs > 0) req.setTransferTimeout(timeoutMs);
    QNetworkReply *reply = m_nam->put(req, data);
    finish(reply, context, std::move(cb));
    return reply;
}

QNetworkReply *ApiClient::uploadMultipart(const QByteArray &data, const QString &name, const QString &mime, const QObject *context, ApiCallback cb) {
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

QNetworkReply *ApiClient::upload(const QByteArray &data, const QString &name, const QString &mime, const QObject *context, ApiCallback cb) {
    const QString sha256 = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
    const ImageThumbMeta meta = buildImageThumb(data, mime);

    QJsonObject body;
    body.insert(QStringLiteral("name"), name);
    body.insert(QStringLiteral("mime"), mime.isEmpty() ? QStringLiteral("application/octet-stream") : mime);
    body.insert(QStringLiteral("size"), qint64(data.size()));
    body.insert(QStringLiteral("sha256"), sha256);
    if (meta.width > 0 && meta.height > 0) {
        body.insert(QStringLiteral("width"), meta.width);
        body.insert(QStringLiteral("height"), meta.height);
    }
    if (!meta.thumbJpeg.isEmpty()) {
        body.insert(QStringLiteral("thumbSize"), qint64(meta.thumbJpeg.size()));
    }

    QPointer<ApiClient> self(this);
    QPointer<const QObject> guard(context);
    return postJson(
        QStringLiteral("/attachments/upload-intent"),
        body,
        context,
        [self, guard, context, data, name, mime, thumbJpeg = meta.thumbJpeg, cb = std::move(cb)](const ApiReply &r) {
            if (!self || (context && !guard)) return;
            if (!r.ok) {
                if (shouldPropagateIntentError(r)) {
                    if (cb) cb(r);
                    return;
                }
                self->uploadMultipart(data, name, mime, context, cb);
                return;
            }
            const QJsonObject obj = r.json.toObject();
            if (obj.value(QLatin1String("duplicate")).toBool() && obj.value(QLatin1String("attachment")).isObject()) {
                if (cb) cb(r);
                return;
            }
            const bool direct = obj.value(QLatin1String("direct")).toBool();
            const QString intentToken = obj.value(QLatin1String("intentToken")).toString();
            const QString uploadUrl = obj.value(QLatin1String("uploadUrl")).toString();
            const QString blobId = obj.value(QLatin1String("blobId")).toString();
            const QByteArray key = QByteArray::fromBase64(obj.value(QLatin1String("key")).toString().toLatin1());
            const qint64 rawChunk = qint64(obj.value(QLatin1String("chunkSize")).toDouble(0));
            const qint64 chunkSize = rawChunk > 0 ? rawChunk : BlobCrypto::kChunkSize;
            const QString thumbUploadUrl = obj.value(QLatin1String("thumbUploadUrl")).toString();
            const QString thumbBlobId = obj.value(QLatin1String("thumbBlobId")).toString();

            if (!direct || intentToken.isEmpty() || uploadUrl.isEmpty() || blobId.isEmpty() || key.size() != 32 || chunkSize <= 0) {
                self->uploadMultipart(data, name, mime, context, cb);
                return;
            }

            QByteArray sealedMain;
            if (!BlobCrypto::encryptBlob(key, blobId, data, &sealedMain, chunkSize)) {
                self->uploadMultipart(data, name, mime, context, cb);
                return;
            }

            auto commit = [self, guard, context, data, name, mime, intentToken, cb](bool hasThumb) {
                if (!self || (context && !guard)) return;
                QJsonObject cbody;
                cbody.insert(QStringLiteral("intentToken"), intentToken);
                cbody.insert(QStringLiteral("hasThumb"), hasThumb);
                self->postJson(
                    QStringLiteral("/attachments/upload-complete"),
                    cbody,
                    context,
                    [self, guard, context, data, name, mime, cb](const ApiReply &cr) {
                        if (!self || (context && !guard)) return;
                        if (cr.ok || shouldPropagateIntentError(cr)) {
                            if (cb) cb(cr);
                            return;
                        }
                        self->uploadMultipart(data, name, mime, context, cb);
                    },
                    60000);
            };

            self->putDirect(
                QUrl(uploadUrl),
                sealedMain,
                context,
                [self, guard, context, data, name, mime, thumbJpeg, key, thumbUploadUrl, thumbBlobId, chunkSize, commit, cb](const ApiReply &pr) {
                    if (!self || (context && !guard)) return;
                    if (!pr.ok) {
                        self->uploadMultipart(data, name, mime, context, cb);
                        return;
                    }
                    if (!thumbJpeg.isEmpty() && !thumbUploadUrl.isEmpty() && !thumbBlobId.isEmpty()) {
                        QByteArray sealedThumb;
                        if (BlobCrypto::encryptBlob(key, thumbBlobId, thumbJpeg, &sealedThumb, chunkSize)) {
                            self->putDirect(
                                QUrl(thumbUploadUrl),
                                sealedThumb,
                                context,
                                [commit](const ApiReply &tr) { commit(tr.ok); },
                                0);
                            return;
                        }
                    }
                    commit(false);
                },
                0);
        },
        30000);
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

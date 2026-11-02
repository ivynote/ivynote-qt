// HTTP access to an Ivynote server (guide 5). Bearer tokens only; cookies are
// neither sent nor stored. Responses come back on the caller's thread.
#pragma once

#include <QByteArray>
#include <QJsonValue>
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;

struct ApiReply {
    bool ok = false;
    int status = 0;  // HTTP status; 0 = network failure (offline, DNS, TLS, timeout)
    QString error;   // server "error" text, or a network description
    QByteArray body;
    QJsonValue json;

    bool network() const { return !ok && status == 0; }
    // 5xx and network failures count as "offline, retry later".
    bool transient() const { return !ok && (status == 0 || status >= 500 || status == 408 || status == 429); }
};

using ApiCallback = std::function<void(const ApiReply &)>;

class ApiClient : public QObject {
    Q_OBJECT
public:
    explicit ApiClient(const QUrl &server, QObject *parent = nullptr);

    QUrl server() const { return m_server; }
    void setToken(const QString &token) { m_token = token; }
    QString token() const { return m_token; }
    void setDevice(const QString &device) { m_device = device; }

    // path is relative to /api/v1 unless it starts with "//" (server root).
    QNetworkReply *send(const QByteArray &method, const QString &path, const QByteArray &body,
                        const QObject *context, ApiCallback cb, int timeoutMs = 30000,
                        const QByteArray &contentType = "application/json");
    QNetworkReply *get(const QString &path, const QObject *context, ApiCallback cb, int timeoutMs = 30000);
    QNetworkReply *postJson(const QString &path, const QJsonValue &body, const QObject *context, ApiCallback cb, int timeoutMs = 30000);
    QNetworkReply *putJson(const QString &path, const QJsonValue &body, const QObject *context, ApiCallback cb);
    QNetworkReply *patchJson(const QString &path, const QJsonValue &body, const QObject *context, ApiCallback cb);
    QNetworkReply *del(const QString &path, const QObject *context, ApiCallback cb);
    QNetworkReply *upload(const QByteArray &data, const QString &name, const QString &mime, const QObject *context, ApiCallback cb);

    QNetworkRequest request(const QString &path, int timeoutMs) const;
    QNetworkAccessManager *nam() const { return m_nam; }

    // "notes.example.com" -> "https://notes.example.com"; strips /api/v1 and
    // trailing slashes. Empty when the text cannot be a server address.
    static QUrl normalizeServer(const QString &input);
    static bool isLocalHost(const QUrl &url);

private:
    void finish(QNetworkReply *reply, const QObject *context, ApiCallback cb);

    QUrl m_server;
    QString m_token;
    QString m_device;
    QNetworkAccessManager *m_nam;
};

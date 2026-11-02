// What the sync engine needs from the network. Callbacks always arrive later
// on the engine's thread, never from inside the call.
#pragma once

#include "core/models.h"

#include <QJsonObject>
#include <QList>
#include <functional>

struct TransportError {
    bool ok = true;
    int status = 0; // 0 with !ok: network failure
    QString message;

    bool network() const { return !ok && status == 0; }
    static TransportError success() { return {}; }
    static TransportError failure(int status, QString message) { return {false, status, std::move(message)}; }
};

class Transport {
public:
    virtual ~Transport() = default;
    virtual void snapshot(std::function<void(TransportError, PullResponse)> done) = 0;
    virtual void pull(qint64 since, std::function<void(TransportError, PullResponse)> done) = 0;
    virtual void push(const QList<QJsonObject> &changes,
                      std::function<void(TransportError, QList<ChangeResult>, qint64 seq)> done) = 0;
    virtual void upload(const QByteArray &data, const QString &name, const QString &mime,
                        std::function<void(TransportError, Attachment)> done) = 0;
};

class ApiClient;

class ApiTransport : public Transport {
public:
    explicit ApiTransport(ApiClient *api) : m_api(api) {}
    void snapshot(std::function<void(TransportError, PullResponse)> done) override;
    void pull(qint64 since, std::function<void(TransportError, PullResponse)> done) override;
    void push(const QList<QJsonObject> &changes,
              std::function<void(TransportError, QList<ChangeResult>, qint64)> done) override;
    void upload(const QByteArray &data, const QString &name, const QString &mime,
                std::function<void(TransportError, Attachment)> done) override;

private:
    void fetch(const QString &path, std::function<void(TransportError, PullResponse)> done);
    ApiClient *m_api;
};

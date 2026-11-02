// Server-Sent Events from /sync/events (guide 6.9). Only a hint that new
// data exists; the data itself still comes from pull, so a lost event costs
// nothing. Reconnects by itself, and treats a silent stream (the server pings
// every 25 s) as dead.
#pragma once

#include <QObject>
#include <QPointer>
#include <QTimer>

class ApiClient;
class QNetworkReply;

class EventStream : public QObject {
    Q_OBJECT
public:
    explicit EventStream(ApiClient *api, QObject *parent = nullptr);
    ~EventStream() override;

    void start();
    void stop();
    void reconnectNow();
    bool connected() const { return m_connected; }

signals:
    void seq(qint64 seq);
    void connectedChanged(bool connected);

private:
    void open();
    void onData();
    void onClosed();
    void setConnected(bool on);

    ApiClient *m_api;
    QPointer<QNetworkReply> m_reply;
    QByteArray m_buffer;
    QString m_event;
    QString m_data;
    QTimer m_retry;
    QTimer m_watchdog;
    int m_delay = 3000;
    bool m_running = false;
    bool m_connected = false;
};

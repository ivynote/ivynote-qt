#include "sync/events.h"

#include "sync/api.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

EventStream::EventStream(ApiClient *api, QObject *parent) : QObject(parent), m_api(api) {
    m_retry.setSingleShot(true);
    connect(&m_retry, &QTimer::timeout, this, &EventStream::open);
    m_watchdog.setSingleShot(true);
    m_watchdog.setInterval(70000);
    connect(&m_watchdog, &QTimer::timeout, this, [this] {
        if (m_reply) m_reply->abort();
    });
}

EventStream::~EventStream() { stop(); }

void EventStream::start() {
    if (m_running) return;
    m_running = true;
    m_delay = 3000;
    open();
}

void EventStream::stop() {
    m_running = false;
    m_retry.stop();
    m_watchdog.stop();
    if (m_reply) {
        QNetworkReply *r = m_reply;
        m_reply = nullptr;
        r->disconnect(this);
        r->abort();
        r->deleteLater();
    }
    setConnected(false);
}

void EventStream::reconnectNow() {
    if (!m_running) return;
    if (m_reply && m_connected) return;
    m_retry.stop();
    m_delay = 3000;
    if (!m_reply) open();
}

void EventStream::open() {
    if (!m_running || m_reply) return;
    QNetworkRequest req = m_api->request(QStringLiteral("/sync/events"), 0);
    req.setRawHeader("Accept", "text/event-stream");
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    m_buffer.clear();
    m_event.clear();
    m_data.clear();
    m_reply = m_api->nam()->get(req);
    connect(m_reply, &QNetworkReply::readyRead, this, &EventStream::onData);
    connect(m_reply, &QNetworkReply::finished, this, &EventStream::onClosed);
    m_watchdog.start();
}

void EventStream::onData() {
    if (!m_reply) return;
    const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status != 200) return; // the finished handler deals with errors
    m_watchdog.start();
    if (!m_connected) {
        m_delay = 3000;
        setConnected(true);
    }
    m_buffer += m_reply->readAll();
    qsizetype nl;
    while ((nl = m_buffer.indexOf('\n')) >= 0) {
        QString line = QString::fromUtf8(m_buffer.left(nl));
        m_buffer.remove(0, nl + 1);
        if (line.endsWith(u'\r')) line.chop(1);
        if (line.isEmpty()) {
            if (m_event == QLatin1String("seq") || (m_event.isEmpty() && !m_data.isEmpty())) {
                bool ok = false;
                const qint64 s = m_data.trimmed().toLongLong(&ok);
                if (ok) emit seq(s);
            }
            m_event.clear();
            m_data.clear();
        } else if (line.startsWith(u':')) {
            // keep-alive comment
        } else if (line.startsWith(QLatin1String("event:"))) {
            m_event = line.mid(6).trimmed();
        } else if (line.startsWith(QLatin1String("data:"))) {
            if (!m_data.isEmpty()) m_data += u'\n';
            m_data += line.mid(5).trimmed();
        }
    }
}

void EventStream::onClosed() {
    QNetworkReply *r = m_reply;
    m_reply = nullptr;
    m_watchdog.stop();
    const int status = r ? r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() : 0;
    if (r) r->deleteLater();
    setConnected(false);
    if (!m_running) return;
    // 401 is the sync engine's business; keep trying slowly in case of a new token.
    m_retry.start(status == 401 ? 60000 : m_delay);
    m_delay = std::min(m_delay * 2, 30000);
}

void EventStream::setConnected(bool on) {
    if (m_connected == on) return;
    m_connected = on;
    emit connectedChanged(on);
}

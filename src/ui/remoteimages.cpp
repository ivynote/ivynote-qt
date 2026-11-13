#include "ui/remoteimages.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QFutureWatcher>
#include <QImageReader>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QStandardPaths>
#include <QUrl>
#include <QtConcurrent>

namespace {
constexpr qint64 maxBytes = 25 * 1024 * 1024;
constexpr int maxSide = 2400; // larger images are scaled down once, on arrival
} // namespace

RemoteImages *RemoteImages::instance() {
    static RemoteImages *s = new RemoteImages(QCoreApplication::instance());
    return s;
}

bool RemoteImages::isRemote(const QString &url) {
    return url.startsWith(QLatin1String("https://"), Qt::CaseInsensitive) || url.startsWith(QLatin1String("http://"), Qt::CaseInsensitive);
}

RemoteImages::RemoteImages(QObject *parent, QNetworkAccessManager *manager) : QObject(parent), m_nam(manager ? manager : new QNetworkAccessManager(this)) {
    m_cache.setMaxCost(256 * 1024);
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (!manager && !dir.isEmpty()) {
        auto *disk = new QNetworkDiskCache(m_nam);
        disk->setCacheDirectory(dir + QStringLiteral("/images"));
        disk->setMaximumCacheSize(200LL * 1024 * 1024);
        m_nam->setCache(disk);
    }
    m_nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    m_nam->setTransferTimeout(30000);
}

QImage RemoteImages::image(const QString &url) {
    if (const QImage *img = m_cache.object(url)) return *img;
    if (!m_failed.contains(url) && !m_pending.contains(url)) fetch(url);
    return {};
}

void RemoteImages::retryFailed() {
    const QSet<QString> failed = m_failed;
    m_failed.clear();
    for (const QString &url : failed) if (isRemote(url)) fetch(url);
}

void RemoteImages::fetch(const QString &url) {
    const QUrl u(url);
    if (!u.isValid() || !isRemote(url)) {
        m_failed.insert(url);
        return;
    }
    m_pending.insert(url);
    QNetworkRequest req(u);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Ivynote/%1").arg(QCoreApplication::applicationVersion()));
    req.setRawHeader("Accept", "image/*");
    QNetworkReply *reply = m_nam->get(req);
    connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 got, qint64 total) {
        if (got > maxBytes || total > maxBytes) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, url] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            finish(url, {});
            return;
        }
        // Decoding a large photo takes long enough to stutter typing.
        auto *watcher = new QFutureWatcher<QImage>(this);
        connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, url] {
            watcher->deleteLater();
            finish(url, watcher->result());
        });
        watcher->setFuture(QtConcurrent::run([data = reply->readAll()] {
            QBuffer buf;
            buf.setData(data);
            buf.open(QIODevice::ReadOnly);
            QImageReader reader(&buf);
            reader.setAutoTransform(true);
            const QSize size = reader.size();
            if (size.isValid() && std::max(size.width(), size.height()) > maxSide)
                reader.setScaledSize(size.scaled(maxSide, maxSide, Qt::KeepAspectRatio));
            QImage img = reader.read();
            if (!img.isNull() && std::max(img.width(), img.height()) > maxSide)
                img = img.scaled(maxSide, maxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            return img;
        }));
    });
}

void RemoteImages::finish(const QString &url, const QImage &img) {
    m_pending.remove(url);
    if (img.isNull()) m_failed.insert(url);
    else m_cache.insert(url, new QImage(img), std::max<qsizetype>(1, img.sizeInBytes() / 1024));
    emit ready(url);
}

// Images written as ![alt](https://…): fetched once in the background, kept
// in a disk cache and a small memory cache, shared by the editor and the
// preview. Nothing is sent but a plain GET; failures are remembered so a
// broken link is not retried on every repaint.
#pragma once

#include <QCache>
#include <QImage>
#include <QObject>
#include <QSet>

class QNetworkAccessManager;

class RemoteImages : public QObject {
    Q_OBJECT
public:
    // An optional borrowed manager allows isolated transports (e.g. tests).
    explicit RemoteImages(QObject *parent = nullptr, QNetworkAccessManager *manager = nullptr);
    static RemoteImages *instance();
    static bool isRemote(const QString &url);

    // The image if it has arrived; otherwise null, and the fetch starts.
    QImage image(const QString &url);
    // Non-triggering memory cache lookup.
    QImage cachedImage(const QString &url) const {
        if (const QImage *img = m_cache.object(url)) return *img;
        return {};
    }
    void retryFailed();
    bool failed(const QString &url) const { return m_failed.contains(url); }

signals:
    // Arrived or failed: either way the caller should look again.
    void ready(const QString &url);

private:
    void fetch(const QString &url);
    void finish(const QString &url, const QImage &img);

    QNetworkAccessManager *m_nam;
    QCache<QString, QImage> m_cache; // cost in KiB
    QSet<QString> m_pending, m_failed;
};

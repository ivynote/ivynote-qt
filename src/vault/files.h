// Files of a vault on disk, sealed by the vault cipher when encrypted:
//   files/<id>       attachments (local vault) or the cache of server files
//   thumbs/<id>      cached server thumbnails
//   uploads/<id>     files staged locally, waiting to be uploaded in the background
//   cache-index.json access metadata for dynamic LRU/heat-based cache eviction
#pragma once

#include "storage/crypto.h"

#include <QByteArray>
#include <QHash>
#include <QString>
#include <functional>

class VaultFiles {
public:
    static constexpr qint64 kDefaultMaxCacheBytes = 500LL * 1024 * 1024;

    struct CacheEntry {
        QString key; // "files/<id>" or "thumbs/<id>"
        QString id;
        bool thumb = false;
        qint64 size = 0;
        qint64 createdAt = 0;
        qint64 lastAccessedAt = 0;
        qint64 accessCount = 1;

        qint64 heatScore() const;
    };

    // Thread-safe snapshot for reading/decrypting cached files off the UI thread.
    struct DiskSource {
        QString root;
        VaultCipher cipher;
        bool allowCacheRead = true;
    };

    VaultFiles(QString root, VaultCipher cipher);
    ~VaultFiles();

    DiskSource diskSource() const { return {m_root, m_cipher, !m_managedCache || m_localFirst}; }
    static bool readRaw(const DiskSource &src, const QString &dir, const QString &id, QByteArray *out);
    void touchAccess(const QString &id, bool thumb);

    // Configures synced cache management (enabled for remote vaults, disabled for
    // local-only vaults where files are the source of truth and never evicted).
    void setCachePolicy(bool managed, bool localFirst, qint64 maxBytes);
    bool isManagedCache() const { return m_managedCache; }
    bool localFirst() const { return m_localFirst; }
    qint64 maxCacheBytes() const { return m_maxCacheBytes; }
    void setClock(std::function<qint64()> now) { m_now = std::move(now); }

    // Runtime alias map: `local-xxx` -> canonical server id after background upload.
    void registerAlias(const QString &localId, const QString &remoteId);
    QString canonicalId(const QString &id) const;

    bool putFile(const QString &id, const QByteArray &data, bool thumb = false);
    bool readFile(const QString &id, QByteArray *out, bool thumb = false);
    bool hasFile(const QString &id, bool thumb = false) const;
    void removeFile(const QString &id);

    // Promotes a finished upload straight into the synced local cache under
    // `remoteId`, avoiding any re-download from the server.
    void promoteUpload(const QString &localId, const QString &remoteId, const QByteArray &data);

    bool putUpload(const QString &id, const QByteArray &data);
    bool readUpload(const QString &id, QByteArray *out) const;
    bool hasUpload(const QString &id) const;
    void removeUpload(const QString &id);

    void evictIfNeeded();
    void clearCache();  // cached server files only; never touches uploads/
    void clearAll();
    qint64 cacheBytes() const;
    int cacheEntryCount() const;
    void flushIndex();

private:
    QString path(const QString &dir, const QString &id) const;
    static QString entryKey(const QString &id, bool thumb);
    void loadIndex();
    void saveIndex();
    void recordEntry(const QString &id, bool thumb, qint64 size);
    qint64 now() const;

    QString m_root;
    VaultCipher m_cipher;
    bool m_managedCache = false;
    bool m_localFirst = true;
    qint64 m_maxCacheBytes = kDefaultMaxCacheBytes;
    bool m_indexLoaded = false;
    bool m_indexDirty = false;
    QHash<QString, CacheEntry> m_entries;
    QHash<QString, QString> m_aliases;
    std::function<qint64()> m_now;
};

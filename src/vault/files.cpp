#include "vault/files.h"

#include "core/ids.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>

qint64 VaultFiles::CacheEntry::heatScore() const {
    const qint64 bonusSteps = std::min<qint64>(std::max<qint64>(accessCount - 1, 0), 14);
    return lastAccessedAt + bonusSteps * (6LL * 3600 * 1000);
}

VaultFiles::VaultFiles(QString root, VaultCipher cipher)
    : m_root(std::move(root)),
      m_cipher(std::move(cipher)),
      m_now([] { return QDateTime::currentMSecsSinceEpoch(); }) {
    for (const char *d : {"files", "thumbs", "uploads"}) QDir().mkpath(m_root + u'/' + QLatin1String(d));
}

VaultFiles::~VaultFiles() {
    flushIndex();
}

qint64 VaultFiles::now() const {
    return m_now ? m_now() : QDateTime::currentMSecsSinceEpoch();
}

QString VaultFiles::path(const QString &dir, const QString &id) const {
    // Ids come from the server; never let one walk out of the directory.
    if (!ids::valid(id)) return {};
    return m_root + u'/' + dir + u'/' + id;
}

QString VaultFiles::entryKey(const QString &id, bool thumb) {
    return (thumb ? QStringLiteral("thumbs/") : QStringLiteral("files/")) + id;
}

void VaultFiles::setCachePolicy(bool managed, bool localFirst, qint64 maxBytes) {
    m_managedCache = managed;
    m_localFirst = localFirst;
    m_maxCacheBytes = std::max<qint64>(0, maxBytes);
    if (m_managedCache && !m_indexLoaded) loadIndex();
    if (m_managedCache) evictIfNeeded();
}

void VaultFiles::registerAlias(const QString &localId, const QString &remoteId) {
    if (!localId.isEmpty() && !remoteId.isEmpty()) m_aliases.insert(localId, remoteId);
}

QString VaultFiles::canonicalId(const QString &id) const {
    return m_aliases.value(id, id);
}

void VaultFiles::loadIndex() {
    m_entries.clear();
    m_indexLoaded = true;
    const QString idxPath = m_root + QStringLiteral("/cache-index.json");
    QByteArray raw;
    if (QFileInfo::exists(idxPath) && m_cipher.readFile(idxPath, QStringLiteral("cache-index.json"), &raw)) {
        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        for (const QJsonValue &v : doc.array()) {
            const QJsonObject o = v.toObject();
            CacheEntry e;
            e.id = o.value(QLatin1String("id")).toString();
            e.thumb = o.value(QLatin1String("thumb")).toBool();
            e.key = entryKey(e.id, e.thumb);
            e.size = qint64(o.value(QLatin1String("size")).toDouble());
            e.createdAt = qint64(o.value(QLatin1String("createdAt")).toDouble());
            e.lastAccessedAt = qint64(o.value(QLatin1String("lastAccessedAt")).toDouble());
            e.accessCount = std::max<qint64>(1, qint64(o.value(QLatin1String("accessCount")).toDouble(1)));
            const QString p = path(e.thumb ? QStringLiteral("thumbs") : QStringLiteral("files"), e.id);
            if (!p.isEmpty() && QFileInfo::exists(p)) {
                e.size = QFileInfo(p).size();
                m_entries.insert(e.key, e);
            }
        }
    }
    // Reconcile files already on disk (e.g. upgraded vaults before cache-index.json existed).
    bool changed = false;
    const qint64 t = now();
    for (bool thumb : {false, true}) {
        const QString dirName = thumb ? QStringLiteral("thumbs") : QStringLiteral("files");
        QDirIterator it(m_root + u'/' + dirName, QDir::Files);
        while (it.hasNext()) {
            const QFileInfo fi = it.nextFileInfo();
            const QString id = fi.fileName();
            if (!ids::valid(id)) continue;
            const QString k = entryKey(id, thumb);
            if (!m_entries.contains(k)) {
                CacheEntry e;
                e.key = k;
                e.id = id;
                e.thumb = thumb;
                e.size = fi.size();
                const qint64 mtime = fi.lastModified().toMSecsSinceEpoch();
                e.createdAt = mtime > 0 ? mtime : t;
                e.lastAccessedAt = e.createdAt;
                e.accessCount = 1;
                m_entries.insert(k, e);
                changed = true;
            }
        }
    }
    if (changed) saveIndex();
}

void VaultFiles::saveIndex() {
    if (!m_managedCache) return;
    QJsonArray arr;
    for (const CacheEntry &e : std::as_const(m_entries)) {
        arr.append(QJsonObject{
            {QStringLiteral("id"), e.id},
            {QStringLiteral("thumb"), e.thumb},
            {QStringLiteral("size"), double(e.size)},
            {QStringLiteral("createdAt"), double(e.createdAt)},
            {QStringLiteral("lastAccessedAt"), double(e.lastAccessedAt)},
            {QStringLiteral("accessCount"), double(e.accessCount)},
        });
    }
    const QString idxPath = m_root + QStringLiteral("/cache-index.json");
    m_cipher.writeFile(idxPath, QJsonDocument(arr).toJson(QJsonDocument::Compact), QStringLiteral("cache-index.json"));
    m_indexDirty = false;
}

void VaultFiles::flushIndex() {
    if (m_managedCache && m_indexDirty) saveIndex();
}

void VaultFiles::recordEntry(const QString &id, bool thumb, qint64 size) {
    if (!m_managedCache) return;
    if (!m_indexLoaded) loadIndex();
    const QString k = entryKey(id, thumb);
    const qint64 t = now();
    auto it = m_entries.find(k);
    if (it != m_entries.end()) {
        it->size = size;
        it->lastAccessedAt = t;
        it->accessCount++;
    } else {
        CacheEntry e;
        e.key = k;
        e.id = id;
        e.thumb = thumb;
        e.size = size;
        e.createdAt = t;
        e.lastAccessedAt = t;
        e.accessCount = 1;
        m_entries.insert(k, e);
    }
    saveIndex();
    evictIfNeeded();
}

bool VaultFiles::putFile(const QString &id, const QByteArray &data, bool thumb) {
    const QString cid = canonicalId(id);
    if (m_managedCache) {
        if (!m_localFirst) return true;
        if (m_maxCacheBytes > 0 && data.size() > m_maxCacheBytes) return true;
    }
    const QString dir = thumb ? QStringLiteral("thumbs") : QStringLiteral("files");
    const QString p = path(dir, cid);
    if (p.isEmpty() || !m_cipher.writeFile(p, data, dir + u'/' + cid)) return false;
    if (m_managedCache) recordEntry(cid, thumb, QFileInfo(p).size());
    return true;
}

bool VaultFiles::readFile(const QString &id, QByteArray *out, bool thumb) {
    const QString cid = canonicalId(id);
    if (m_managedCache && !m_localFirst) return false;
    const QString dir = thumb ? QStringLiteral("thumbs") : QStringLiteral("files");
    const QString p = path(dir, cid);
    if (p.isEmpty() || !QFileInfo::exists(p)) {
        if (m_managedCache && m_indexLoaded && m_entries.remove(entryKey(cid, thumb)) > 0) saveIndex();
        return false;
    }
    if (!m_cipher.readFile(p, dir + u'/' + cid, out)) return false;
    if (m_managedCache) {
        if (!m_indexLoaded) loadIndex();
        const QString k = entryKey(cid, thumb);
        const qint64 t = now();
        auto it = m_entries.find(k);
        if (it != m_entries.end()) {
            it->lastAccessedAt = t;
            it->accessCount++;
            m_indexDirty = true;
        } else {
            recordEntry(cid, thumb, QFileInfo(p).size());
        }
    }
    return true;
}

bool VaultFiles::hasFile(const QString &id, bool thumb) const {
    const QString cid = canonicalId(id);
    if (m_managedCache && !m_localFirst) return false;
    const QString p = path(thumb ? QStringLiteral("thumbs") : QStringLiteral("files"), cid);
    return !p.isEmpty() && QFileInfo::exists(p);
}

void VaultFiles::removeFile(const QString &id) {
    const QString cid = canonicalId(id);
    bool removed = false;
    for (bool thumb : {false, true}) {
        const QString dir = thumb ? QStringLiteral("thumbs") : QStringLiteral("files");
        const QString p = path(dir, cid);
        if (!p.isEmpty()) QFile::remove(p);
        if (m_managedCache && m_indexLoaded && m_entries.remove(entryKey(cid, thumb)) > 0) removed = true;
    }
    if (removed) saveIndex();
}

void VaultFiles::promoteUpload(const QString &localId, const QString &remoteId, const QByteArray &data) {
    registerAlias(localId, remoteId);
    if (m_managedCache && !m_localFirst) return;
    // Already cached (e.g. duplicate upload): just refresh its access metadata.
    if (hasFile(remoteId, false)) {
        if (m_managedCache) {
            if (!m_indexLoaded) loadIndex();
            auto it = m_entries.find(entryKey(remoteId, false));
            if (it != m_entries.end()) {
                it->lastAccessedAt = now();
                it->accessCount++;
                saveIndex();
                return;
            }
        } else {
            return;
        }
    }
    putFile(remoteId, data, false);
}

bool VaultFiles::putUpload(const QString &id, const QByteArray &data) {
    const QString p = path(QStringLiteral("uploads"), id);
    return !p.isEmpty() && m_cipher.writeFile(p, data, QStringLiteral("uploads/") + id);
}

bool VaultFiles::readUpload(const QString &id, QByteArray *out) const {
    const QString p = path(QStringLiteral("uploads"), id);
    return !p.isEmpty() && QFileInfo::exists(p) && m_cipher.readFile(p, QStringLiteral("uploads/") + id, out);
}

bool VaultFiles::hasUpload(const QString &id) const {
    const QString p = path(QStringLiteral("uploads"), id);
    return !p.isEmpty() && QFileInfo::exists(p);
}

void VaultFiles::removeUpload(const QString &id) {
    const QString p = path(QStringLiteral("uploads"), id);
    if (!p.isEmpty()) QFile::remove(p);
}

void VaultFiles::evictIfNeeded() {
    if (!m_managedCache) return;
    if (!m_indexLoaded) loadIndex();
    if (!m_localFirst) {
        if (!m_entries.isEmpty() || cacheBytes() > 0) clearCache();
        return;
    }
    if (m_maxCacheBytes <= 0) return;
    qint64 total = 0;
    for (const CacheEntry &e : std::as_const(m_entries)) total += e.size;
    if (total <= m_maxCacheBytes) return;

    QList<CacheEntry> sorted = m_entries.values();
    std::sort(sorted.begin(), sorted.end(), [](const CacheEntry &a, const CacheEntry &b) {
        const qint64 ha = a.heatScore();
        const qint64 hb = b.heatScore();
        if (ha != hb) return ha < hb;
        if (a.lastAccessedAt != b.lastAccessedAt) return a.lastAccessedAt < b.lastAccessedAt;
        if (a.size != b.size) return a.size > b.size;
        return a.key < b.key;
    });

    bool changed = false;
    for (const CacheEntry &e : sorted) {
        if (total <= m_maxCacheBytes) break;
        const QString p = path(e.thumb ? QStringLiteral("thumbs") : QStringLiteral("files"), e.id);
        if (!p.isEmpty()) QFile::remove(p);
        m_entries.remove(e.key);
        total -= e.size;
        changed = true;
    }
    if (changed) saveIndex();
}

void VaultFiles::clearCache() {
    for (const char *d : {"files", "thumbs"}) {
        QDir dir(m_root + u'/' + QLatin1String(d));
        dir.removeRecursively();
        QDir().mkpath(dir.path());
    }
    m_entries.clear();
    m_indexDirty = false;
    QFile::remove(m_root + QStringLiteral("/cache-index.json"));
}

void VaultFiles::clearAll() {
    clearCache();
    m_aliases.clear();
    QDir dir(m_root + QStringLiteral("/uploads"));
    dir.removeRecursively();
    QDir().mkpath(dir.path());
}

qint64 VaultFiles::cacheBytes() const {
    if (m_managedCache && m_indexLoaded) {
        qint64 total = 0;
        for (const CacheEntry &e : std::as_const(m_entries)) total += e.size;
        return total;
    }
    qint64 total = 0;
    for (const char *d : {"files", "thumbs"}) {
        QDirIterator it(m_root + u'/' + QLatin1String(d), QDir::Files);
        while (it.hasNext()) total += it.nextFileInfo().size();
    }
    return total;
}

int VaultFiles::cacheEntryCount() const {
    if (m_managedCache && m_indexLoaded) return int(m_entries.size());
    int count = 0;
    for (const char *d : {"files", "thumbs"}) {
        QDirIterator it(m_root + u'/' + QLatin1String(d), QDir::Files);
        while (it.hasNext()) {
            it.next();
            count++;
        }
    }
    return count;
}

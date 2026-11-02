#include "vault/files.h"

#include "core/ids.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

VaultFiles::VaultFiles(QString root, VaultCipher cipher) : m_root(std::move(root)), m_cipher(std::move(cipher)) {
    for (const char *d : {"files", "thumbs", "uploads"}) QDir().mkpath(m_root + u'/' + QLatin1String(d));
}

QString VaultFiles::path(const QString &dir, const QString &id) const {
    // Ids come from the server; never let one walk out of the directory.
    if (!ids::valid(id)) return {};
    return m_root + u'/' + dir + u'/' + id;
}

bool VaultFiles::putFile(const QString &id, const QByteArray &data, bool thumb) {
    const QString dir = thumb ? QStringLiteral("thumbs") : QStringLiteral("files");
    const QString p = path(dir, id);
    return !p.isEmpty() && m_cipher.writeFile(p, data, dir + u'/' + id);
}

bool VaultFiles::readFile(const QString &id, QByteArray *out, bool thumb) const {
    const QString dir = thumb ? QStringLiteral("thumbs") : QStringLiteral("files");
    const QString p = path(dir, id);
    return !p.isEmpty() && QFileInfo::exists(p) && m_cipher.readFile(p, dir + u'/' + id, out);
}

bool VaultFiles::hasFile(const QString &id, bool thumb) const {
    const QString p = path(thumb ? QStringLiteral("thumbs") : QStringLiteral("files"), id);
    return !p.isEmpty() && QFileInfo::exists(p);
}

void VaultFiles::removeFile(const QString &id) {
    for (const char *d : {"files", "thumbs"}) {
        const QString p = path(QLatin1String(d), id);
        if (!p.isEmpty()) QFile::remove(p);
    }
}

bool VaultFiles::putUpload(const QString &id, const QByteArray &data) {
    const QString p = path(QStringLiteral("uploads"), id);
    return !p.isEmpty() && m_cipher.writeFile(p, data, QStringLiteral("uploads/") + id);
}

bool VaultFiles::readUpload(const QString &id, QByteArray *out) const {
    const QString p = path(QStringLiteral("uploads"), id);
    return !p.isEmpty() && QFileInfo::exists(p) && m_cipher.readFile(p, QStringLiteral("uploads/") + id, out);
}

void VaultFiles::removeUpload(const QString &id) {
    const QString p = path(QStringLiteral("uploads"), id);
    if (!p.isEmpty()) QFile::remove(p);
}

void VaultFiles::clearCache() {
    for (const char *d : {"files", "thumbs"}) {
        QDir dir(m_root + u'/' + QLatin1String(d));
        dir.removeRecursively();
        QDir().mkpath(dir.path());
    }
}

void VaultFiles::clearAll() {
    clearCache();
    QDir dir(m_root + QStringLiteral("/uploads"));
    dir.removeRecursively();
    QDir().mkpath(dir.path());
}

qint64 VaultFiles::cacheBytes() const {
    qint64 total = 0;
    for (const char *d : {"files", "thumbs"}) {
        QDirIterator it(m_root + u'/' + QLatin1String(d), QDir::Files);
        while (it.hasNext()) total += it.nextFileInfo().size();
    }
    return total;
}

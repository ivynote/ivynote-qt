// Files of a vault on disk, sealed by the vault cipher when encrypted:
//   files/<id>    attachments (local vault) or the cache of server files
//   thumbs/<id>   cached server thumbnails
//   uploads/<id>  files inserted offline, waiting to be uploaded
#pragma once

#include "storage/crypto.h"

#include <QByteArray>
#include <QString>

class VaultFiles {
public:
    VaultFiles(QString root, VaultCipher cipher);

    bool putFile(const QString &id, const QByteArray &data, bool thumb = false);
    bool readFile(const QString &id, QByteArray *out, bool thumb = false) const;
    bool hasFile(const QString &id, bool thumb = false) const;
    void removeFile(const QString &id);

    bool putUpload(const QString &id, const QByteArray &data);
    bool readUpload(const QString &id, QByteArray *out) const;
    void removeUpload(const QString &id);

    void clearCache();  // cached server files only
    void clearAll();
    qint64 cacheBytes() const;

private:
    QString path(const QString &dir, const QString &id) const;

    QString m_root;
    VaultCipher m_cipher;
};

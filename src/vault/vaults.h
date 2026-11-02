// The vaults on this device. A vault is either local-only or bound to one
// account on one server; that binding is fixed at creation, so a local vault
// never turns into a synced one and their data never meet (moving notes
// between them is an explicit import).
#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>

struct VaultInfo {
    enum Kind { Local, Remote };

    QString id;
    QString name;
    Kind kind = Local;
    bool encrypted = false;
    qint64 createdAt = 0;
    qint64 openedAt = 0;
    // Remote vaults
    QString server;
    QString siteName;
    QString userId;
    QString username;
    QJsonObject user; // last known /me, for working offline

    bool isRemote() const { return kind == Remote; }
    QString dir() const;
    QString tokenKey() const { return QStringLiteral("token:") + id; }
    QString vaultKeyKey() const { return QStringLiteral("vault-key:") + id; }

    QJsonObject toJson() const;
    static VaultInfo fromJson(const QJsonObject &o);
};

namespace vaults {

QString dataRoot();
QList<VaultInfo> list();
VaultInfo find(const QString &id);
void save(const VaultInfo &v); // insert or update
void remove(const QString &id); // deletes its data and secrets
QString lastOpened();
void setLastOpened(const QString &id);
QString deviceId();

// Creates the vault directory and key. Returns false and leaves nothing
// behind on failure.
bool create(VaultInfo &v, QString *error);

// For tests: keep everything under dir.
void setDataRoot(const QString &dir);

} // namespace vaults

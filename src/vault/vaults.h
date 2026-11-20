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
    QString path; // optional custom directory for local vaults (used directly, no subdir)
    // Remote vaults
    QString server;
    QString siteName;
    QString userId;
    QString username;
    QJsonObject user; // last known /me, for working offline

    bool isRemote() const { return kind == Remote; }
    bool hasCustomPath() const { return !isRemote() && !path.isEmpty(); }
    QString dir() const;
    QString tokenKey() const { return QStringLiteral("token:") + id; }
    QString vaultKeyKey() const { return QStringLiteral("vault-key:") + id; }

    QJsonObject toJson() const;
    static VaultInfo fromJson(const QJsonObject &o);
};

struct DiscoveredVault {
    bool valid = false;
    QString dir;
    QString id;
    QString name;
    bool encrypted = false;
    bool isRemote = false;
    int noteRecords = 0;
    qint64 createdAt = 0;
    qint64 modifiedAt = 0;
    QString registeredId; // non-empty if already in vaults.json
};

namespace vaults {

QString dataRoot();
QString defaultDataRoot(); // matches Tauri app_data_dir (app.ivynote.desktop)
QString legacyDataRoot();  // previous Qt AppDataLocation (Ivynote/Ivynote)
bool migrateLegacyData();
bool migrateLegacyData(const QString &fromDir, const QString &toDir);
QList<VaultInfo> list();
VaultInfo find(const QString &id);
void save(const VaultInfo &v);                      // insert or update
void remove(const QString &id, bool deleteFiles = true); // unlinks and optionally deletes data
QString lastOpened();
void setLastOpened(const QString &id);
QString deviceId();

// Writes self-describing vault.json inside v.dir() so the vault can be
// reliably identified and recovered later.
bool writeMetadata(const VaultInfo &v);

// Creates the vault directory and key. When v.path is set (local vaults),
// v.path itself is used directly without creating a subdirectory. Returns
// false and leaves nothing behind on failure.
bool create(VaultInfo &v, QString *error);

// Read-only verification of whether dir is a valid Ivynote vault.
DiscoveredVault inspectDir(const QString &dir);

// Registers an existing local vault directory into vaults.json (or returns
// the existing entry if already registered).
bool attachExisting(const QString &dir, const QString &nameOverride = {}, VaultInfo *out = nullptr, QString *error = nullptr);

// Scans the default vaults directory (dataRoot()/vaults) and registers any
// valid unregistered local vaults found there.
QList<VaultInfo> discoverDefaultVaults();

// Bounded scan for valid local Ivynote vaults. When extraRoots is non-empty,
// scans only those roots; otherwise scans default, legacy, common user dirs,
// and mounted data drives.
QList<DiscoveredVault> scanForVaults(const QStringList &extraRoots = {});

// Moves a closed local vault's database, metadata, and files directly into
// newDir and updates its registered path.
bool relocate(const QString &id, const QString &newDir, QString *error);

// For tests: keep everything under dir.
void setDataRoot(const QString &dir);

} // namespace vaults

#include "vault/vaults.h"

#include "core/ids.h"
#include "storage/crypto.h"
#include "storage/secrets.h"
#include "vault/backup.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>

namespace {

QString g_root;

QString registryPath() { return vaults::dataRoot() + QStringLiteral("/vaults.json"); }

QJsonObject readRegistry() {
    QFile f(registryPath());
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

void writeRegistry(const QJsonObject &o) {
    QDir().mkpath(vaults::dataRoot());
    QSaveFile f(registryPath());
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(QJsonDocument(o).toJson());
    f.commit();
}

bool copyRecursive(const QString &src, const QString &dst) {
    const QFileInfo info(src);
    if (!info.exists()) return false;
    if (info.isDir()) {
        if (!QDir().mkpath(dst)) return false;
        const QDir dir(src);
        for (const QFileInfo &entry : dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)) {
            if (!copyRecursive(entry.absoluteFilePath(), dst + u'/' + entry.fileName())) return false;
        }
        return true;
    }
    QDir().mkpath(QFileInfo(dst).absolutePath());
    QFile::remove(dst);
    return QFile::copy(src, dst);
}

bool moveEntry(const QString &src, const QString &dst) {
    if (!QFileInfo::exists(src)) return true;
    QDir().mkpath(QFileInfo(dst).absolutePath());
    if (QFile::rename(src, dst)) return true;
    // Cross-filesystem fallback: copy completely before removing the source.
    if (!copyRecursive(src, dst)) return false;
    if (QFileInfo(src).isDir()) QDir(src).removeRecursively();
    else QFile::remove(src);
    return true;
}

} // namespace

QString VaultInfo::dir() const { return vaults::dataRoot() + QStringLiteral("/vaults/") + id; }

QJsonObject VaultInfo::toJson() const {
    return QJsonObject{{"id", id}, {"name", name}, {"kind", kind == Remote ? "remote" : "local"},
                       {"encrypted", encrypted}, {"createdAt", double(createdAt)}, {"openedAt", double(openedAt)},
                       {"server", server}, {"siteName", siteName}, {"userId", userId},
                       {"username", username}, {"user", user}};
}

VaultInfo VaultInfo::fromJson(const QJsonObject &o) {
    VaultInfo v;
    v.id = o.value(QLatin1String("id")).toString();
    v.name = o.value(QLatin1String("name")).toString();
    v.kind = o.value(QLatin1String("kind")).toString() == QLatin1String("remote") ? Remote : Local;
    v.encrypted = o.value(QLatin1String("encrypted")).toBool();
    v.createdAt = qint64(o.value(QLatin1String("createdAt")).toDouble());
    v.openedAt = qint64(o.value(QLatin1String("openedAt")).toDouble());
    v.server = o.value(QLatin1String("server")).toString();
    v.siteName = o.value(QLatin1String("siteName")).toString();
    v.userId = o.value(QLatin1String("userId")).toString();
    v.username = o.value(QLatin1String("username")).toString();
    v.user = o.value(QLatin1String("user")).toObject();
    return v;
}

namespace vaults {

void setDataRoot(const QString &dir) { g_root = dir; }

QString defaultDataRoot() {
    // Match Tauri's app_data_dir() (tauri.conf.json identifier = "app.ivynote.desktop"):
    //   Linux:   $XDG_DATA_HOME/app.ivynote.desktop  (~/.local/share/app.ivynote.desktop)
    //   macOS:   ~/Library/Application Support/app.ivynote.desktop
    //   Windows: %APPDATA%/app.ivynote.desktop       (Roaming)
#if defined(Q_OS_WIN)
    QString base = qEnvironmentVariable("APPDATA");
    if (base.isEmpty()) {
        QDir d(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
        if (!QCoreApplication::applicationName().isEmpty()) d.cdUp();
        if (!QCoreApplication::organizationName().isEmpty()) d.cdUp();
        base = d.absolutePath();
    }
#else
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
#endif
    return QDir::cleanPath(base + QStringLiteral("/app.ivynote.desktop"));
}

QString legacyDataRoot() {
    return QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
}

QString dataRoot() {
    if (!g_root.isEmpty()) return g_root;
    return defaultDataRoot();
}

bool migrateLegacyData(const QString &fromDir, const QString &toDir) {
    const QString src = QDir::cleanPath(fromDir);
    const QString dst = QDir::cleanPath(toDir);
    if (src.isEmpty() || dst.isEmpty() || src == dst) return false;
    const QString srcReg = src + QStringLiteral("/vaults.json");
    const QString dstReg = dst + QStringLiteral("/vaults.json");
    if (!QFileInfo::exists(srcReg) || QFileInfo::exists(dstReg)) return false;

    // Never move files out from under an older instance still running on the legacy path.
    QLockFile legacyLock(src + QStringLiteral("/ivynote.lock"));
    legacyLock.setStaleLockTime(0);
    if (!legacyLock.tryLock(0)) return false;

    if (!QDir().mkpath(dst)) return false;

    const QString srcVaults = src + QStringLiteral("/vaults");
    const QString dstVaults = dst + QStringLiteral("/vaults");
    if (QFileInfo::exists(srcVaults)) {
        if (!QFileInfo::exists(dstVaults)) {
            if (!moveEntry(srcVaults, dstVaults)) return false;
        } else {
            const QDir d(srcVaults);
            for (const QFileInfo &entry : d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                const QString targetVault = dstVaults + u'/' + entry.fileName();
                if (!QFileInfo::exists(targetVault) && !moveEntry(entry.absoluteFilePath(), targetVault)) return false;
            }
            if (d.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty()) QDir().rmdir(srcVaults);
        }
    }

    const QString srcSecrets = src + QStringLiteral("/secrets.json");
    const QString dstSecrets = dst + QStringLiteral("/secrets.json");
    if (QFileInfo::exists(srcSecrets) && !QFileInfo::exists(dstSecrets)) {
        if (!moveEntry(srcSecrets, dstSecrets)) return false;
        QFile::setPermissions(dstSecrets, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }

    // Move the registry last so any interrupted step stays retryable without losing data.
    if (!moveEntry(srcReg, dstReg)) return false;

    legacyLock.unlock();
    QDir().rmdir(src);
    return true;
}

bool migrateLegacyData() {
    if (!g_root.isEmpty()) return false;
    return migrateLegacyData(legacyDataRoot(), defaultDataRoot());
}

QList<VaultInfo> list() {
    QList<VaultInfo> out;
    for (const auto &v : readRegistry().value(QLatin1String("vaults")).toArray()) out << VaultInfo::fromJson(v.toObject());
    return out;
}

VaultInfo find(const QString &id) {
    for (const auto &v : list())
        if (v.id == id) return v;
    return {};
}

void save(const VaultInfo &v) {
    QJsonObject reg = readRegistry();
    QJsonArray arr = reg.value(QLatin1String("vaults")).toArray();
    bool found = false;
    for (int i = 0; i < arr.size(); ++i) {
        if (arr[i].toObject().value(QLatin1String("id")).toString() == v.id) {
            arr[i] = v.toJson();
            found = true;
        }
    }
    if (!found) arr.append(v.toJson());
    reg.insert(QStringLiteral("vaults"), arr);
    writeRegistry(reg);
}

void remove(const QString &id) {
    const VaultInfo v = find(id);
    QJsonObject reg = readRegistry();
    QJsonArray arr = reg.value(QLatin1String("vaults")).toArray();
    for (int i = arr.size() - 1; i >= 0; --i)
        if (arr[i].toObject().value(QLatin1String("id")).toString() == id) arr.removeAt(i);
    reg.insert(QStringLiteral("vaults"), arr);
    if (reg.value(QLatin1String("last")).toString() == id) reg.remove(QStringLiteral("last"));
    writeRegistry(reg);
    if (!v.id.isEmpty() && ids::valid(v.id)) {
        QDir(v.dir()).removeRecursively();
        secrets::remove(v.tokenKey());
        secrets::remove(v.vaultKeyKey());
        backup::removeConfig(v.id);
    }
}

QString lastOpened() { return readRegistry().value(QLatin1String("last")).toString(); }

void setLastOpened(const QString &id) {
    QJsonObject reg = readRegistry();
    reg.insert(QStringLiteral("last"), id);
    writeRegistry(reg);
}

QString deviceId() {
    QJsonObject reg = readRegistry();
    QString dev = reg.value(QLatin1String("device")).toString();
    if (dev.isEmpty()) {
#if defined(Q_OS_WIN)
        const QString prefix = QStringLiteral("win");
#elif defined(Q_OS_MACOS)
        const QString prefix = QStringLiteral("mac");
#else
        const QString prefix = QStringLiteral("linux");
#endif
        dev = prefix + u'-' + ids::newId(6);
        reg.insert(QStringLiteral("device"), dev);
        writeRegistry(reg);
    }
    return dev;
}

bool create(VaultInfo &v, QString *error) {
    if (v.id.isEmpty()) v.id = ids::newId();
    if (v.createdAt == 0) v.createdAt = QDateTime::currentMSecsSinceEpoch();
    if (!QDir().mkpath(v.dir())) {
        if (error) *error = QStringLiteral("cannot create %1").arg(v.dir());
        return false;
    }
    if (v.encrypted && !secrets::set(v.vaultKeyKey(), VaultCipher::generateKey())) {
        if (error) *error = QStringLiteral("cannot store the vault key");
        QDir(v.dir()).removeRecursively();
        return false;
    }
    save(v);
    return true;
}

} // namespace vaults

#include "vault/vaults.h"

#include "core/ids.h"
#include "storage/crypto.h"
#include "storage/secrets.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
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

QString dataRoot() {
    if (!g_root.isEmpty()) return g_root;
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
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

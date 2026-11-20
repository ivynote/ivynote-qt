#include "vault/vaults.h"

#include "core/i18n.h"
#include "core/ids.h"
#include "storage/crypto.h"
#include "storage/secrets.h"
#include "vault/backup.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QQueue>
#include <QSaveFile>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QSysInfo>
#include <algorithm>

namespace {

QString g_root;

constexpr Qt::CaseSensitivity kPathCase =
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    Qt::CaseInsensitive;
#else
    Qt::CaseSensitive;
#endif

const QStringList kVaultFiles{
    QStringLiteral("vault.db"),
    QStringLiteral("vault.db-wal"),
    QStringLiteral("vault.db-shm"),
    QStringLiteral("vault.db-journal"),
    QStringLiteral("vault.json"),
};

const QStringList kVaultSubdirs{
    QStringLiteral("files"),
    QStringLiteral("thumbs"),
    QStringLiteral("uploads"),
};

QString registryPath() { return vaults::dataRoot() + QStringLiteral("/vaults.json"); }

QString normalizePath(const QString &path) {
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) return {};
    return QDir::cleanPath(QDir(trimmed).absolutePath());
}

QString canonicalKey(const QString &path) {
    const QString norm = normalizePath(path);
    if (norm.isEmpty()) return {};
    QFileInfo fi(norm);
    if (fi.exists()) {
        const QString canon = fi.canonicalFilePath();
        const QString out = canon.isEmpty() ? norm : QDir::cleanPath(canon);
        return kPathCase == Qt::CaseInsensitive ? out.toLower() : out;
    }
    // Walk up to the deepest existing ancestor so symlinks in parent dirs match.
    QStringList tail;
    QDir cur(norm);
    while (!cur.exists()) {
        const QString name = cur.dirName();
        if (name.isEmpty() || !cur.cdUp()) break;
        tail.prepend(name);
    }
    QString base = cur.exists() ? QFileInfo(cur.absolutePath()).canonicalFilePath() : norm;
    if (base.isEmpty()) base = cur.absolutePath();
    for (const QString &seg : tail) base += u'/' + seg;
    const QString out = QDir::cleanPath(base);
    return kPathCase == Qt::CaseInsensitive ? out.toLower() : out;
}

bool sameDir(const QString &a, const QString &b) {
    const QString ka = canonicalKey(a);
    const QString kb = canonicalKey(b);
    return !ka.isEmpty() && ka == kb;
}

bool isSubpathOrSame(const QString &child, const QString &parent) {
    const QString kc = canonicalKey(child);
    const QString kp = canonicalKey(parent);
    if (kc.isEmpty() || kp.isEmpty()) return false;
    if (kc == kp) return true;
    const QString prefix = kp.endsWith(u'/') ? kp : (kp + u'/');
    return kc.startsWith(prefix);
}

bool dirsOverlap(const QString &a, const QString &b) {
    return isSubpathOrSame(a, b) || isSubpathOrSame(b, a);
}

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

bool testWritableDir(const QString &dir) {
    const QString probe = dir + QStringLiteral("/.ivynote-write-test-") + ids::newId(6);
    QFile f(probe);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.close();
    QFile::remove(probe);
    return true;
}

bool isDirEmpty(const QString &dir) {
    const QDir d(dir);
    return d.exists() && d.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty();
}

bool validateTargetPath(const QString &rawDir, QString *error) {
    const QString target = normalizePath(rawDir);
    if (target.isEmpty()) {
        if (error) *error = T("请选择有效的文件夹。");
        return false;
    }
    if (QDir(target).isRoot()) {
        if (error) *error = T("不能将磁盘根目录直接作为笔记库位置，请新建或选择一个子文件夹。");
        return false;
    }
    const QString root = vaults::dataRoot();
    if (sameDir(target, root) || sameDir(target, root + QStringLiteral("/vaults"))) {
        if (error) *error = T("不能使用应用数据根目录作为笔记库位置，请选择其他文件夹。");
        return false;
    }
    for (const VaultInfo &existing : vaults::list()) {
        if (dirsOverlap(target, existing.dir())) {
            if (error) *error = T("所选位置与现有笔记库「%1」重合，请选择其他文件夹。").arg(existing.name);
            return false;
        }
    }
    if (QFileInfo::exists(target + QStringLiteral("/vault.db"))) {
        if (error) *error = T("所选文件夹中已存在笔记库（vault.db）。如需打开它，请使用「打开已有本地笔记库」。");
        return false;
    }
    return true;
}

bool hasSqliteHeader(const QString &dbPath) {
    QFile f(dbPath);
    if (!f.exists() || f.size() < 100 || !f.open(QIODevice::ReadOnly)) return false;
    const QByteArray head = f.read(16);
    static const QByteArray kMagic("SQLite format 3\0", 16);
    return head == kMagic;
}

bool tableHasColumns(QSqlDatabase &db, const QString &table, const QStringList &required) {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) return false;
    QSet<QString> cols;
    while (q.next()) cols.insert(q.value(1).toString());
    for (const QString &col : required)
        if (!cols.contains(col)) return false;
    return true;
}

bool shouldSkipScanDirName(const QString &name) {
    if (name.isEmpty() || name.startsWith(u'.')) return true;
    static const QSet<QString> kSkip{
        QStringLiteral("node_modules"),
        QStringLiteral("build"),
        QStringLiteral("dist"),
        QStringLiteral("target"),
        QStringLiteral("vendor"),
        QStringLiteral("venv"),
        QStringLiteral("__pycache__"),
        QStringLiteral("library"),
        QStringLiteral("applications"),
        QStringLiteral("appdata"),
        QStringLiteral("program files"),
        QStringLiteral("program files (x86)"),
        QStringLiteral("programdata"),
        QStringLiteral("windows"),
        QStringLiteral("$recycle.bin"),
        QStringLiteral("system volume information"),
        QStringLiteral("proc"),
        QStringLiteral("sys"),
        QStringLiteral("dev"),
        QStringLiteral("run"),
        QStringLiteral("tmp"),
        QStringLiteral("var"),
        QStringLiteral("usr"),
        QStringLiteral("etc"),
        QStringLiteral("boot"),
        QStringLiteral("snap"),
        QStringLiteral("flatpak"),
        QStringLiteral("ivynotebackups"),
    };
    return kSkip.contains(name.toLower());
}

bool isSystemMountPath(const QString &mountPath) {
    const QString clean = QDir::cleanPath(mountPath);
#if defined(Q_OS_WIN)
    return clean.isEmpty();
#elif defined(Q_OS_MACOS)
    return clean == QStringLiteral("/") || !clean.startsWith(QStringLiteral("/Volumes/"));
#else
    if (clean == QStringLiteral("/")) return true;
    static const QStringList prefixes{
        QStringLiteral("/proc"), QStringLiteral("/sys"), QStringLiteral("/dev"),
        QStringLiteral("/boot"), QStringLiteral("/snap"), QStringLiteral("/var"),
        QStringLiteral("/tmp"), QStringLiteral("/usr"), QStringLiteral("/etc"),
        QStringLiteral("/efi"),
    };
    for (const QString &p : prefixes) {
        if (clean == p || clean.startsWith(p + u'/')) return true;
    }
    if (clean == QStringLiteral("/run") || (clean.startsWith(QStringLiteral("/run/")) && !clean.startsWith(QStringLiteral("/run/media/")))) {
        return true;
    }
    return false;
#endif
}

QList<QPair<QString, int>> defaultScanRoots() {
    QList<QPair<QString, int>> roots;
    QSet<QString> seen;
    const auto addRoot = [&](const QString &path, int maxDepth) {
        const QString norm = normalizePath(path);
        if (norm.isEmpty() || !QFileInfo(norm).isDir()) return;
        const QString key = canonicalKey(norm);
        if (seen.contains(key)) return;
        seen.insert(key);
        roots.append({norm, maxDepth});
    };

    addRoot(vaults::dataRoot() + QStringLiteral("/vaults"), 1);
    if (!g_root.isEmpty()) {
        // Test mode: keep automatic scans strictly inside the isolated test root.
        return roots;
    }

    const QString legacy = vaults::legacyDataRoot();
    if (!sameDir(legacy, vaults::dataRoot())) {
        addRoot(legacy + QStringLiteral("/vaults"), 1);
    }

    addRoot(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation), 3);
    addRoot(QStandardPaths::writableLocation(QStandardPaths::DesktopLocation), 2);
    addRoot(QDir::homePath(), 2);

    for (const QStorageInfo &vol : QStorageInfo::mountedVolumes()) {
        if (!vol.isValid() || !vol.isReady()) continue;
        const QString rootPath = vol.rootPath();
        if (isSystemMountPath(rootPath)) continue;
        const QByteArray fs = vol.fileSystemType().toLower();
        if (fs == "tmpfs" || fs == "devtmpfs" || fs == "proc" || fs == "sysfs" || fs == "squashfs" || fs == "overlay" || fs == "autofs")
            continue;
        addRoot(rootPath, 2);
    }
    return roots;
}

} // namespace

QString VaultInfo::dir() const {
    if (!isRemote() && !path.isEmpty()) return QDir::cleanPath(path);
    return vaults::dataRoot() + QStringLiteral("/vaults/") + id;
}

QJsonObject VaultInfo::toJson() const {
    QJsonObject o{{"id", id}, {"name", name}, {"kind", kind == Remote ? "remote" : "local"},
                  {"encrypted", encrypted}, {"createdAt", double(createdAt)}, {"openedAt", double(openedAt)},
                  {"server", server}, {"siteName", siteName}, {"userId", userId},
                  {"username", username}, {"user", user}};
    if (!isRemote() && !path.isEmpty()) o.insert(QStringLiteral("path"), QDir::cleanPath(path));
    return o;
}

VaultInfo VaultInfo::fromJson(const QJsonObject &o) {
    VaultInfo v;
    v.id = o.value(QLatin1String("id")).toString();
    v.name = o.value(QLatin1String("name")).toString();
    v.kind = o.value(QLatin1String("kind")).toString() == QLatin1String("remote") ? Remote : Local;
    v.encrypted = o.value(QLatin1String("encrypted")).toBool();
    v.createdAt = qint64(o.value(QLatin1String("createdAt")).toDouble());
    v.openedAt = qint64(o.value(QLatin1String("openedAt")).toDouble());
    const QString customPath = o.value(QLatin1String("path")).toString().trimmed();
    if (!v.isRemote() && !customPath.isEmpty()) v.path = QDir::cleanPath(customPath);
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

bool writeMetadata(const VaultInfo &v) {
    if (v.id.isEmpty()) return false;
    const QString dir = v.dir();
    if (!QFileInfo(dir).isDir()) return false;
    QSaveFile f(dir + QStringLiteral("/vault.json"));
    if (!f.open(QIODevice::WriteOnly)) return false;
    const QJsonObject meta{
        {"format", QStringLiteral("ivynote-vault")},
        {"version", 1},
        {"id", v.id},
        {"name", v.name},
        {"kind", v.isRemote() ? QStringLiteral("remote") : QStringLiteral("local")},
        {"encrypted", v.encrypted},
        {"createdAt", double(v.createdAt)},
    };
    f.write(QJsonDocument(meta).toJson(QJsonDocument::Indented));
    return f.commit();
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
    if (QFileInfo(v.dir()).isDir()) writeMetadata(v);
}

void remove(const QString &id, bool deleteFiles) {
    const VaultInfo v = find(id);
    QJsonObject reg = readRegistry();
    QJsonArray arr = reg.value(QLatin1String("vaults")).toArray();
    for (int i = arr.size() - 1; i >= 0; --i)
        if (arr[i].toObject().value(QLatin1String("id")).toString() == id) arr.removeAt(i);
    reg.insert(QStringLiteral("vaults"), arr);
    if (reg.value(QLatin1String("last")).toString() == id) reg.remove(QStringLiteral("last"));
    writeRegistry(reg);
    if (!v.id.isEmpty() && ids::valid(v.id)) {
        if (deleteFiles) {
            if (!v.hasCustomPath()) {
                QDir(v.dir()).removeRecursively();
            } else {
                // For custom directories, only remove known Ivynote artifacts so unrelated
                // user files in the same folder are never harmed.
                const QString dir = v.dir();
                for (const QString &file : kVaultFiles) QFile::remove(dir + u'/' + file);
                for (const QString &sub : kVaultSubdirs) QDir(dir + u'/' + sub).removeRecursively();
                if (isDirEmpty(dir)) QDir().rmdir(dir);
            }
        }
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

    if (!v.isRemote() && !v.path.trimmed().isEmpty()) {
        const QString norm = normalizePath(v.path);
        if (!validateTargetPath(norm, error)) return false;
        const QString defaultDir = dataRoot() + QStringLiteral("/vaults/") + v.id;
        v.path = sameDir(norm, defaultDir) ? QString() : norm;
    } else if (v.isRemote()) {
        v.path.clear();
    }

    const QString targetDir = v.dir();
    const bool dirExisted = QFileInfo::exists(targetDir);
    if (!QDir().mkpath(targetDir)) {
        if (error) *error = T("无法创建目录：%1").arg(targetDir);
        return false;
    }
    if (!testWritableDir(targetDir)) {
        if (error) *error = T("目录不可写入：%1").arg(targetDir);
        if (!dirExisted && isDirEmpty(targetDir)) QDir().rmdir(targetDir);
        return false;
    }
    if (v.encrypted && !secrets::set(v.vaultKeyKey(), VaultCipher::generateKey())) {
        if (error) *error = T("无法保存笔记库密钥。");
        if (!dirExisted) QDir(targetDir).removeRecursively();
        return false;
    }
    if (!writeMetadata(v)) {
        if (error) *error = T("无法写入笔记库信息文件：%1").arg(targetDir);
        if (v.encrypted) secrets::remove(v.vaultKeyKey());
        if (!dirExisted && isDirEmpty(targetDir)) QDir().rmdir(targetDir);
        return false;
    }
    save(v);
    return true;
}

DiscoveredVault inspectDir(const QString &rawDir) {
    DiscoveredVault out;
    const QString cleanDir = normalizePath(rawDir);
    if (cleanDir.isEmpty() || !QFileInfo(cleanDir).isDir()) return out;

    const QString dbPath = cleanDir + QStringLiteral("/vault.db");
    if (!hasSqliteHeader(dbPath)) return out;

    const QString conn = QStringLiteral("inspect-") + ids::newId(8);
    bool schemaOk = false;
    bool encryptedFromDb = false;
    bool remoteFromDb = false;
    int noteRecords = 0;

    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (db.open()) {
            QSet<QString> tables;
            {
                QSqlQuery q(db);
                if (q.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type='table'"))) {
                    while (q.next()) tables.insert(q.value(0).toString());
                }
            }
            static const QStringList kRequiredTables{
                QStringLiteral("notes"), QStringLiteral("folders"), QStringLiteral("tags"),
                QStringLiteral("attachments"), QStringLiteral("meta"),
            };
            bool hasAll = true;
            for (const QString &t : kRequiredTables) {
                if (!tables.contains(t)) {
                    hasAll = false;
                    break;
                }
            }
            if (hasAll && tableHasColumns(db, QStringLiteral("notes"), {QStringLiteral("id"), QStringLiteral("data")}) &&
                tableHasColumns(db, QStringLiteral("meta"), {QStringLiteral("key"), QStringLiteral("value")})) {
                schemaOk = true;
                {
                    QSqlQuery q(db);
                    if (q.exec(QStringLiteral("SELECT key, value FROM meta WHERE key IN ('keycheck','cursor','lastSyncAt')"))) {
                        while (q.next()) {
                            const QString key = q.value(0).toString();
                            const QString val = q.value(1).toString();
                            if (key == QLatin1String("keycheck") && !val.isEmpty()) encryptedFromDb = true;
                            if ((key == QLatin1String("cursor") || key == QLatin1String("lastSyncAt")) && val.toLongLong() > 0)
                                remoteFromDb = true;
                        }
                    }
                }
                {
                    QSqlQuery q(db);
                    if (q.exec(QStringLiteral("SELECT COUNT(*) FROM notes")) && q.next()) {
                        noteRecords = q.value(0).toInt();
                    }
                }
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(conn);

    if (!schemaOk) return out;

    out.valid = true;
    out.dir = cleanDir;
    out.encrypted = encryptedFromDb;
    out.isRemote = remoteFromDb;
    out.noteRecords = noteRecords;
    out.modifiedAt = QFileInfo(dbPath).lastModified().toMSecsSinceEpoch();

    // Read optional self-describing vault.json.
    QFile metaFile(cleanDir + QStringLiteral("/vault.json"));
    if (metaFile.open(QIODevice::ReadOnly)) {
        const QJsonObject meta = QJsonDocument::fromJson(metaFile.readAll()).object();
        const QString format = meta.value(QLatin1String("format")).toString();
        if (format == QLatin1String("ivynote-vault") || meta.contains(QLatin1String("kind"))) {
            const QString mid = meta.value(QLatin1String("id")).toString();
            if (ids::valid(mid)) out.id = mid;
            const QString mname = meta.value(QLatin1String("name")).toString().trimmed();
            if (!mname.isEmpty()) out.name = mname;
            if (meta.value(QLatin1String("kind")).toString() == QLatin1String("remote")) out.isRemote = true;
            else if (meta.value(QLatin1String("kind")).toString() == QLatin1String("local") && !remoteFromDb) out.isRemote = false;
            if (meta.value(QLatin1String("encrypted")).toBool()) out.encrypted = true;
            const qint64 created = qint64(meta.value(QLatin1String("createdAt")).toDouble());
            if (created > 0) out.createdAt = created;
        }
    }

    // Match against registered vaults in vaults.json.
    for (const VaultInfo &reg : list()) {
        if (sameDir(reg.dir(), cleanDir)) {
            out.registeredId = reg.id;
            if (out.id.isEmpty()) out.id = reg.id;
            if (out.name.isEmpty()) out.name = reg.name;
            out.isRemote = reg.isRemote();
            if (out.createdAt == 0) out.createdAt = reg.createdAt;
            if (reg.encrypted) out.encrypted = true;
            break;
        }
    }

    if (out.name.isEmpty()) {
        const QString folderName = QFileInfo(cleanDir).fileName();
        const QString defaultVaultsDir = dataRoot() + QStringLiteral("/vaults");
        const QString parentDir = QFileInfo(cleanDir).absolutePath();
        if (sameDir(parentDir, defaultVaultsDir) && ids::valid(folderName)) {
            out.id = out.id.isEmpty() ? folderName : out.id;
            out.name = T("我的笔记");
        } else {
            out.name = folderName.isEmpty() ? T("本地笔记库") : folderName;
        }
    }
    return out;
}

bool attachExisting(const QString &dir, const QString &nameOverride, VaultInfo *out, QString *error) {
    const DiscoveredVault d = inspectDir(dir);
    if (!d.valid) {
        if (error) *error = T("所选文件夹不是有效的 Ivynote 笔记库（未找到有效的 vault.db）。");
        return false;
    }
    if (d.isRemote) {
        if (error) *error = T("该目录是同步笔记库的本地缓存，不能作为本地笔记库打开。请使用「连接 Ivynote 服务器」登录。");
        return false;
    }
    if (!d.registeredId.isEmpty()) {
        VaultInfo existing = find(d.registeredId);
        if (!existing.id.isEmpty()) {
            const QString customName = nameOverride.trimmed().left(60);
            if (!customName.isEmpty() && customName != existing.name) {
                existing.name = customName;
                save(existing);
            }
            if (out) *out = existing;
            return true;
        }
    }

    VaultInfo v;
    v.kind = VaultInfo::Local;
    const QString folderName = QFileInfo(d.dir).fileName();
    const QString defaultVaultsDir = dataRoot() + QStringLiteral("/vaults");
    const bool inDefaultRoot = sameDir(QFileInfo(d.dir).absolutePath(), defaultVaultsDir);

    if (inDefaultRoot && ids::valid(folderName) && find(folderName).id.isEmpty()) {
        v.id = folderName;
    } else if (!d.id.isEmpty() && ids::valid(d.id) && find(d.id).id.isEmpty()) {
        v.id = d.id;
    } else {
        v.id = ids::newId();
    }

    const QString defaultDir = defaultVaultsDir + u'/' + v.id;
    v.path = sameDir(d.dir, defaultDir) ? QString() : d.dir;

    const QString chosenName = nameOverride.trimmed().isEmpty() ? d.name : nameOverride.trimmed();
    v.name = chosenName.left(60);
    if (v.name.isEmpty()) v.name = T("本地笔记库");
    v.encrypted = d.encrypted;
    v.createdAt = d.createdAt > 0 ? d.createdAt : (d.modifiedAt > 0 ? d.modifiedAt : QDateTime::currentMSecsSinceEpoch());
    save(v);
    if (out) *out = v;
    return true;
}

QList<VaultInfo> discoverDefaultVaults() {
    QList<VaultInfo> added;
    const QDir vaultsDir(dataRoot() + QStringLiteral("/vaults"));
    if (!vaultsDir.exists()) return added;
    for (const QFileInfo &entry : vaultsDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const DiscoveredVault d = inspectDir(entry.absoluteFilePath());
        if (!d.valid || d.isRemote || !d.registeredId.isEmpty()) continue;
        VaultInfo attached;
        if (attachExisting(d.dir, QString(), &attached, nullptr)) added << attached;
    }
    return added;
}

QList<DiscoveredVault> scanForVaults(const QStringList &extraRoots) {
    QList<QPair<QString, int>> roots;
    if (!extraRoots.isEmpty()) {
        for (const QString &r : extraRoots) {
            const QString norm = normalizePath(r);
            if (!norm.isEmpty() && QFileInfo(norm).isDir()) roots.append({norm, 3});
        }
    } else {
        roots = defaultScanRoots();
    }

    QList<DiscoveredVault> results;
    QSet<QString> visited;
    QSet<QString> foundKeys;

    QElapsedTimer timer;
    timer.start();
    constexpr int kMaxDirs = 4000;
    constexpr qint64 kMaxMs = 2000;
    int scanned = 0;

    struct Item {
        QString dir;
        int depth;
        int maxDepth;
    };
    QQueue<Item> queue;
    for (const auto &[root, maxDepth] : roots) {
        const QString key = canonicalKey(root);
        if (key.isEmpty() || visited.contains(key)) continue;
        visited.insert(key);
        queue.enqueue({root, 0, maxDepth});
    }

    while (!queue.isEmpty() && scanned < kMaxDirs && timer.elapsed() < kMaxMs) {
        const Item cur = queue.dequeue();
        ++scanned;

        if (QFileInfo::exists(cur.dir + QStringLiteral("/vault.db"))) {
            const DiscoveredVault d = inspectDir(cur.dir);
            if (d.valid) {
                if (!d.isRemote) {
                    const QString key = canonicalKey(d.dir);
                    if (!foundKeys.contains(key)) {
                        foundKeys.insert(key);
                        results.append(d);
                    }
                }
                // Never descend inside a valid vault (files/, thumbs/, uploads/).
                continue;
            }
        }

        if (cur.depth >= cur.maxDepth) continue;
        const QDir dir(cur.dir);
        const QFileInfoList subdirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks | QDir::Readable);
        for (const QFileInfo &sub : subdirs) {
            if (shouldSkipScanDirName(sub.fileName())) continue;
            const QString subPath = sub.absoluteFilePath();
            const QString key = canonicalKey(subPath);
            if (key.isEmpty() || visited.contains(key)) continue;
            visited.insert(key);
            queue.enqueue({subPath, cur.depth + 1, cur.maxDepth});
        }
    }

    std::sort(results.begin(), results.end(), [](const DiscoveredVault &a, const DiscoveredVault &b) {
        const bool aReg = !a.registeredId.isEmpty();
        const bool bReg = !b.registeredId.isEmpty();
        if (aReg != bReg) return !aReg; // unregistered vaults first
        if (a.modifiedAt != b.modifiedAt) return a.modifiedAt > b.modifiedAt;
        return a.dir < b.dir;
    });
    return results;
}

} // namespace vaults

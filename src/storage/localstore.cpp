#include "storage/localstore.h"

#include "core/ids.h"

#include <QJsonDocument>
#include <QLoggingCategory>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

Q_LOGGING_CATEGORY(lcStore, "ivy.store")

namespace {

QString tableName(Table t) {
    switch (t) {
    case Table::Notes: return QStringLiteral("notes");
    case Table::Folders: return QStringLiteral("folders");
    case Table::Tags: return QStringLiteral("tags");
    case Table::Attachments: return QStringLiteral("attachments");
    case Table::Outbox: return QStringLiteral("outbox");
    case Table::Uploads: return QStringLiteral("uploads");
    }
    return {};
}

const Table kTables[] = {Table::Notes, Table::Folders, Table::Tags, Table::Attachments, Table::Outbox, Table::Uploads};

QByteArray recordAd(Table t, const QString &id) {
    return (QStringLiteral("ivy:rec:v1|") + tableName(t) + u'|' + id).toUtf8();
}

const QByteArray kCheckPlain("ivynote-vault-key");

} // namespace

LocalStore::LocalStore(QString path, VaultCipher cipher)
    : m_path(std::move(path)), m_conn(QStringLiteral("vault-") + ids::newId(8)), m_cipher(std::move(cipher)) {}

LocalStore::~LocalStore() { close(); }

bool LocalStore::isOpen() const { return QSqlDatabase::contains(m_conn) && QSqlDatabase::database(m_conn, false).isOpen(); }

LocalStore::OpenResult LocalStore::open(QString *error) {
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_conn);
    db.setDatabaseName(m_path);
    if (!db.open()) {
        if (error) *error = db.lastError().text();
        return OpenResult::Failed;
    }
    {
        QSqlQuery q(db);
        q.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
        q.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
        bool ok = true;
        for (Table t : kTables)
            ok = ok && q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS %1 (id TEXT PRIMARY KEY, data BLOB NOT NULL)").arg(tableName(t)));
        ok = ok && q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)"));
        if (!ok) {
            if (error) *error = q.lastError().text();
            q.finish();
            db = QSqlDatabase();
            close();
            return OpenResult::Failed;
        }
    }
    db = QSqlDatabase();
    // A sealed marker tells a wrong key apart from damaged rows.
    const QString check = meta(QStringLiteral("keycheck"));
    if (m_cipher.active()) {
        if (check.isEmpty()) {
            setMeta(QStringLiteral("keycheck"), QString::fromLatin1(m_cipher.seal(kCheckPlain, "keycheck").toBase64()));
        } else {
            QByteArray plain;
            if (!m_cipher.open(QByteArray::fromBase64(check.toLatin1()), "keycheck", &plain) || plain != kCheckPlain) {
                if (error) *error = QStringLiteral("wrong vault key");
                close();
                return OpenResult::WrongKey;
            }
        }
    } else if (!check.isEmpty()) {
        if (error) *error = QStringLiteral("vault is encrypted");
        close();
        return OpenResult::WrongKey;
    }
    return OpenResult::Ok;
}

void LocalStore::close() {
    if (!QSqlDatabase::contains(m_conn)) return;
    {
        QSqlDatabase db = QSqlDatabase::database(m_conn, false);
        if (db.isOpen()) db.close();
    }
    QSqlDatabase::removeDatabase(m_conn);
}

QList<QJsonObject> LocalStore::all(Table table) {
    QList<QJsonObject> out;
    QSqlQuery q(QSqlDatabase::database(m_conn));
    if (!q.exec(QStringLiteral("SELECT id, data FROM %1").arg(tableName(table)))) {
        qCWarning(lcStore) << "read failed:" << q.lastError().text();
        return out;
    }
    while (q.next()) {
        const QString id = q.value(0).toString();
        QByteArray plain;
        if (!m_cipher.open(q.value(1).toByteArray(), recordAd(table, id), &plain)) {
            m_unreadable++;
            qCWarning(lcStore) << "unreadable record in" << tableName(table);
            continue;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(plain);
        if (doc.isObject()) out << doc.object();
    }
    return out;
}

bool LocalStore::apply(const StoreBatch &batch) {
    if (batch.isEmpty()) return true;
    QSqlDatabase db = QSqlDatabase::database(m_conn);
    if (!db.transaction()) return false;
    QSqlQuery q(db);
    bool ok = true;
    for (const auto &p : batch.puts) {
        q.prepare(QStringLiteral("INSERT OR REPLACE INTO %1 (id, data) VALUES (?, ?)").arg(tableName(p.table)));
        q.addBindValue(p.id);
        q.addBindValue(m_cipher.seal(QJsonDocument(p.data).toJson(QJsonDocument::Compact), recordAd(p.table, p.id)));
        ok = q.exec() && ok;
    }
    for (const auto &d : batch.dels) {
        q.prepare(QStringLiteral("DELETE FROM %1 WHERE id = ?").arg(tableName(d.table)));
        q.addBindValue(d.id);
        ok = q.exec() && ok;
    }
    for (auto it = batch.meta.begin(); it != batch.meta.end(); ++it) {
        q.prepare(QStringLiteral("INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?)"));
        q.addBindValue(it.key());
        q.addBindValue(it.value());
        ok = q.exec() && ok;
    }
    if (!ok) {
        qCWarning(lcStore) << "write failed:" << q.lastError().text();
        db.rollback();
        return false;
    }
    return db.commit();
}

QString LocalStore::meta(const QString &key, const QString &fallback) {
    QSqlQuery q(QSqlDatabase::database(m_conn));
    q.prepare(QStringLiteral("SELECT value FROM meta WHERE key = ?"));
    q.addBindValue(key);
    if (q.exec() && q.next()) return q.value(0).toString();
    return fallback;
}

bool LocalStore::setMeta(const QString &key, const QString &value) {
    StoreBatch b;
    b.meta.insert(key, value);
    return apply(b);
}

bool LocalStore::clear() {
    QSqlDatabase db = QSqlDatabase::database(m_conn);
    QSqlQuery q(db);
    db.transaction();
    for (Table t : kTables) q.exec(QStringLiteral("DELETE FROM %1").arg(tableName(t)));
    q.exec(QStringLiteral("DELETE FROM meta WHERE key <> 'keycheck'"));
    return db.commit();
}

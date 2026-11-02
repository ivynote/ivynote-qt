// SQLite persistence of a vault: one row per entity holding its JSON,
// sealed by the vault cipher when the vault is encrypted. Everything is read
// into memory at start-up (guide 4.4); writes come in batched transactions.
#pragma once

#include "storage/crypto.h"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>

enum class Table { Notes, Folders, Tags, Attachments, Outbox, Uploads };

struct StoreBatch {
    struct Put {
        Table table;
        QString id;
        QJsonObject data;
    };
    struct Del {
        Table table;
        QString id;
    };
    QList<Put> puts;
    QList<Del> dels;
    QHash<QString, QString> meta;

    bool isEmpty() const { return puts.isEmpty() && dels.isEmpty() && meta.isEmpty(); }
};

class LocalStore {
public:
    enum class OpenResult { Ok, WrongKey, Failed };

    LocalStore(QString path, VaultCipher cipher);
    ~LocalStore();
    LocalStore(const LocalStore &) = delete;
    LocalStore &operator=(const LocalStore &) = delete;

    OpenResult open(QString *error = nullptr);
    void close();
    bool isOpen() const;

    QList<QJsonObject> all(Table table);
    bool apply(const StoreBatch &batch);
    QString meta(const QString &key, const QString &fallback = {});
    bool setMeta(const QString &key, const QString &value);
    bool clear();

    const VaultCipher &cipher() const { return m_cipher; }
    int unreadable() const { return m_unreadable; }

private:
    QString m_path;
    QString m_conn;
    VaultCipher m_cipher;
    int m_unreadable = 0;
};

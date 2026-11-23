// An opened vault: its store, files, engine and, for a synced vault, the
// server connection, live event stream and attachment downloads.
#pragma once

#include "core/models.h"
#include "vault/vaults.h"

#include <QCache>
#include <QImage>
#include <QObject>
#include <QSet>
#include <QThreadPool>
#include <atomic>
#include <functional>
#include <memory>

class ApiClient;
class ApiTransport;
class EventStream;
class LocalStore;
class SyncEngine;
class VaultFiles;

class Session : public QObject {
    Q_OBJECT
public:
    enum class OpenError { None, MissingKey, WrongKey, Failed };

    // keyOverride: a recovery key entered by the user, used when the
    // keychain has lost the vault key; stored back on success.
    static Session *open(const VaultInfo &info, const QByteArray &keyOverride, OpenError *err, QString *message,
                         QObject *parent = nullptr);
    ~Session() override;

    const VaultInfo &info() const { return m_info; }
    SyncEngine *engine() const { return m_engine; }
    ApiClient *api() const { return m_api; }
    VaultFiles *files() const { return m_files; }
    bool isRemote() const { return m_info.isRemote(); }
    bool signedIn() const;
    User user() const { return m_user; }
    bool liveConnected() const;
    QByteArray vaultKey() const;
    int unreadableRecords() const;

    void start();   // first sync, live events, refresh /me
    void wake();    // back in the foreground or online again
    void rename(const QString &name);
    // After signing in again: only the same account may take over the vault.
    bool applySignIn(const QString &token, const User &user, QString *error);
    // Pushes what it can, ends the server session, then reports back. The
    // caller removes the vault afterwards.
    void signOut(std::function<void()> done);
    void close();

    // Attachments: null until loaded; attachmentReady follows.
    void applyCachePolicy(bool localFirst, qint64 maxBytes);
    QImage image(const QString &id, bool thumb);
    // Non-triggering memory cache lookup (no disk read, decryption, or network fetch).
    QImage cachedImage(const QString &id, bool thumb) const;
    void fileData(const QString &id, std::function<void(bool ok, QByteArray data)> done);
    void openAttachment(const QString &id); // with the system viewer
    void saveAttachmentAs(const QString &id, QWidget *parent);

signals:
    void attachmentReady(const QString &id);
    void needsSignIn();
    void userChanged();
    void liveChanged(bool connected);

private:
    explicit Session(const VaultInfo &info, QObject *parent);
    void refreshMe();
    void fetch(const QString &id, bool thumb, std::function<void(bool, QByteArray)> done);
    static QImage decode(const QByteArray &data, int maxSide);

    VaultInfo m_info;
    User m_user;
    LocalStore *m_store = nullptr;
    VaultFiles *m_files = nullptr;
    ApiClient *m_api = nullptr;
    ApiTransport *m_transport = nullptr;
    EventStream *m_events = nullptr;
    SyncEngine *m_engine = nullptr;
    QCache<QString, QImage> m_images;
    QSet<QString> m_loading;
    QSet<QString> m_failedImages;
    QHash<QString, QList<std::function<void(bool, QByteArray)>>> m_waiting;
    QThreadPool m_imagePool;
    std::shared_ptr<std::atomic<bool>> m_cancelled = std::make_shared<std::atomic<bool>>(false);
    bool m_closed = false;
};

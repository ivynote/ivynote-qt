// Local-first sync engine (guide 6; web site/src/lib/sync/engine.ts).
//
// All reads come from memory, loaded from the local store at start-up. Every
// local edit is applied at once, persisted shortly after, and queued in an
// outbox that is pushed in the background. Pulls bring everything after the
// last seen sequence number; fields still waiting in the outbox win over
// pulled ones until they are pushed. Note bodies carry the revision they
// were based on, so the server can three-way merge concurrent edits.
//
// Without a transport the engine runs a local-only vault: the same data and
// operations, no outbox and no network.
#pragma once

#include "core/hlc.h"
#include "core/models.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <functional>
#include <optional>

class LocalStore;
class Transport;
class VaultFiles;
struct TransportError;

struct SyncState {
    enum Status { Idle, Syncing, Offline, Error, Local };
    bool ready = false;
    Status status = Idle;
    int pending = 0;
    int blocked = 0; // changes the server refused, kept locally
    int uploads = 0;
    qint64 lastSyncAt = 0;
    QString error;
};

struct NotePatch {
    std::optional<QString> title;
    std::optional<QString> body;
    std::optional<QString> folderId;
    std::optional<QStringList> tags;
    std::optional<bool> pinned;
    std::optional<bool> trashed;
};

struct NoteInit {
    QString title;
    QString body;
    QString folderId;
    QStringList tags;
    bool pinned = false;
    qint64 createdAt = 0; // 0 = now; imports keep their dates
    QString id;           // "" = new random id; imports pre-assign ids to link notes
};

struct AddedFile {
    bool ok = false;
    QString id;
    QString kind;
    QString name;
    bool local = false; // queued for upload
    QString error;
};

class SyncEngine : public QObject {
    Q_OBJECT
public:
    SyncEngine(LocalStore *store, VaultFiles *files, Transport *transport, QString device, QObject *parent = nullptr);
    ~SyncEngine() override;

    struct Options {
        int debounceMs = 800;
        int persistMs = 300;
        std::function<qint64()> now;
    };
    void setOptions(const Options &o);

    bool init();
    bool isLocalOnly() const { return m_transport == nullptr; }
    QString device() const { return m_device; }

    // --- sync ---
    void sync();                           // push and pull now, coalescing calls
    void scheduleSync(int delayMs = -1);   // default: the edit debounce
    void onServerSeq(qint64 seq);
    void stop();
    // Signed out (401): no network until resumed, edits keep queuing.
    void pause();
    void resume();
    bool paused() const { return m_paused; }
    bool running() const { return m_running; }
    const SyncState &state() const { return m_state; }
    qint64 cursor() const { return m_cursor; }

    // --- data ---
    const QHash<QString, Note> &notes() const { return m_notes; }
    const QHash<QString, Folder> &folders() const { return m_folders; }
    const QHash<QString, Tag> &tags() const { return m_tags; }
    const QHash<QString, Attachment> &attachments() const { return m_attachments; }
    const QHash<QString, OutboxEntry> &outbox() const { return m_outbox; }
    const QHash<QString, PendingUpload> &uploads() const { return m_uploads; }
    const Note *note(const QString &id) const;
    bool hasPending(EntityType type, const QString &id) const;
    QList<const Note *> noteList() const;
    QList<const Folder *> folderList() const;
    QList<const Tag *> tagList() const;
    QList<const Folder *> folderPath(const QString &folderId) const; // root first

    // --- local mutations ---
    Note createNote(const NoteInit &init = {});
    void updateNote(const QString &id, const NotePatch &patch);
    void deleteNote(const QString &id);
    Folder createFolder(const QString &name, const QString &parentId = {});
    void updateFolder(const QString &id, const std::optional<QString> &name, const std::optional<QString> &parentId,
                      const std::optional<double> &sort = std::nullopt);
    bool canMoveFolder(const QString &id, const QString &parent) const;
    void deleteFolder(const QString &id);
    Tag createTag(const QString &name, const QString &color = {});
    void updateTag(const QString &id, const std::optional<QString> &name, const std::optional<QString> &color);
    void deleteTag(const QString &id);

    // --- attachments ---
    void addFile(const QByteArray &data, const QString &name, const QString &mime, std::function<void(AddedFile)> done);
    QString attachmentKind(const QString &id) const;
    QString attachmentName(const QString &id) const;

    bool persistNow();
    void reset(); // clears everything (logout)

signals:
    // Emitted (synchronously) right before server text is applied, so an
    // editor can hand over keystrokes it has not saved yet. They then wait in
    // the outbox and get merged instead of being overwritten.
    void aboutToApplyRemote();
    void dataChanged();                       // coalesced, ~120 ms
    void structureChanged();                  // immediately: tree-affecting change
    void stateChanged();
    void remoteBody(const QString &noteId, const QString &body);
    void resourceNormalized(const QString &localId, const QString &remoteId);
    void conflict(const QString &noteId, const QString &copyId);
    void rejected(const QString &type, const QString &id, const QString &error);
    void uploadFailed(const QString &name, const QString &error);
    void unauthorized();
    void roundFinished(bool ok);

private:
    using Done = std::function<void(const TransportError &)>;

    qint64 now() const { return m_now(); }
    void bump(bool immediate = false);
    void refreshState();
    void setStatus(SyncState::Status s, const QString &error = {});
    void schedulePersist();
    void enqueue(EntityType type, const QString &id, const QJsonObject &fields, bool isDelete = false,
                 std::optional<qint64> baseRev = std::nullopt);

    void runOnce();
    void finishRound(const TransportError &err);
    void flushUploads(Done done);
    void flushNextUpload(Done done);
    void push(Done done);
    void pushNext(QStringList keys, int index, Done done);
    void sendBatch(QList<OutboxEntry> batch, Done done);
    void applyResult(const OutboxEntry &sent, const ChangeResult &r);
    void pull(Done done);
    void applyPull(const PullResponse &res, bool snapshot);
    template <typename T>
    void applyRemote(EntityType type, const T &remote, QHash<QString, T> &map, QSet<QString> &dirty);

    LocalStore *m_store;
    VaultFiles *m_files;
    Transport *m_transport;
    QString m_device;
    Hlc m_hlc;
    std::function<qint64()> m_now;
    int m_debounceMs = 800;
    int m_persistMs = 300;

    QHash<QString, Note> m_notes;
    QHash<QString, Folder> m_folders;
    QHash<QString, Tag> m_tags;
    QHash<QString, Attachment> m_attachments;
    QHash<QString, OutboxEntry> m_outbox;
    QHash<QString, PendingUpload> m_uploads;
    qint64 m_cursor = 0;
    qint64 m_savedCursor = 0;
    SyncState m_state;

    struct Dirty {
        QSet<QString> notes, folders, tags, attachments, outbox, uploads;
        bool any() const;
    } m_dirty;

    QTimer m_syncTimer, m_persistTimer, m_notifyTimer;
    int m_retryDelay = 2000;
    bool m_running = false;
    bool m_again = false;
    bool m_stopped = false;
    bool m_paused = false;
};

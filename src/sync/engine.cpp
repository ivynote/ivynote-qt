#include "sync/engine.h"

#include "core/ids.h"
#include "core/markdown.h"
#include "storage/localstore.h"
#include "sync/transport.h"
#include "vault/files.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QImageReader>
#include <QBuffer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QPointer>
#include <algorithm>

Q_LOGGING_CATEGORY(lcSync, "ivy.sync")

namespace {

constexpr int kPushBatch = 200;
// Below what the server reads; one larger change still goes out alone.
constexpr qsizetype kPushBytes = 6 << 20;

QString keyOf(EntityType t, const QString &id) { return typeName(t) + u':' + id; }

// The server answered that sending the same request again is pointless.
bool refused(const TransportError &e) {
    return !e.ok && e.status >= 400 && e.status < 500 && e.status != 401 && e.status != 408 && e.status != 429;
}

QJsonValue toJsonValue(const QStringList &l) { return QJsonArray::fromStringList(l); }

} // namespace

bool SyncEngine::Dirty::any() const {
    return !notes.isEmpty() || !folders.isEmpty() || !tags.isEmpty() || !attachments.isEmpty() || !outbox.isEmpty() || !uploads.isEmpty();
}

SyncEngine::SyncEngine(LocalStore *store, VaultFiles *files, Transport *transport, QString device, QObject *parent)
    : QObject(parent), m_store(store), m_files(files), m_transport(transport), m_device(std::move(device)),
      m_hlc(m_device), m_now([] { return QDateTime::currentMSecsSinceEpoch(); }) {
    m_syncTimer.setSingleShot(true);
    m_persistTimer.setSingleShot(true);
    m_notifyTimer.setSingleShot(true);
    m_notifyTimer.setInterval(120);
    connect(&m_syncTimer, &QTimer::timeout, this, &SyncEngine::sync);
    connect(&m_persistTimer, &QTimer::timeout, this, [this] { persistNow(); });
    connect(&m_notifyTimer, &QTimer::timeout, this, &SyncEngine::dataChanged);
    m_state.status = transport ? SyncState::Idle : SyncState::Local;
}

SyncEngine::~SyncEngine() {
    m_stopped = true;
    persistNow();
}

void SyncEngine::setOptions(const Options &o) {
    m_debounceMs = o.debounceMs;
    m_persistMs = o.persistMs;
    if (o.now) {
        m_now = o.now;
        m_hlc = Hlc(m_device, o.now);
    }
}

bool SyncEngine::init() {
    for (const auto &o : m_store->all(Table::Notes)) {
        Note n = Note::fromJson(o);
        m_notes.insert(n.id, n);
    }
    for (const auto &o : m_store->all(Table::Folders)) {
        Folder f = Folder::fromJson(o);
        m_folders.insert(f.id, f);
    }
    for (const auto &o : m_store->all(Table::Tags)) {
        Tag t = Tag::fromJson(o);
        m_tags.insert(t.id, t);
    }
    for (const auto &o : m_store->all(Table::Attachments)) {
        Attachment a = Attachment::fromJson(o);
        m_attachments.insert(a.id, a);
    }
    for (const auto &o : m_store->all(Table::Outbox)) {
        // A restart is a new attempt: the clock or the server may have changed,
        // so refusals are not kept (blocked is never persisted).
        OutboxEntry e = OutboxEntry::fromJson(o);
        m_outbox.insert(e.key, e);
    }
    for (const auto &o : m_store->all(Table::Uploads)) {
        PendingUpload u = PendingUpload::fromJson(o);
        m_uploads.insert(u.id, u);
    }
    // Remember the newest local stamp so new edits always sort after old ones.
    for (const auto &e : std::as_const(m_outbox)) m_hlc.observe(e.hlc);
    m_cursor = m_savedCursor = m_store->meta(QStringLiteral("cursor"), QStringLiteral("0")).toLongLong();
    m_state.lastSyncAt = m_store->meta(QStringLiteral("lastSyncAt"), QStringLiteral("0")).toLongLong();
    m_state.ready = true;
    refreshState();
    bump(true);
    return true;
}

// --- state ---

void SyncEngine::bump(bool immediate) {
    if (immediate) {
        m_notifyTimer.stop();
        emit structureChanged();
        emit dataChanged();
        return;
    }
    if (!m_notifyTimer.isActive()) m_notifyTimer.start();
}

void SyncEngine::refreshState() {
    int blocked = 0;
    for (const auto &e : std::as_const(m_outbox))
        if (!e.blocked.isEmpty()) blocked++;
    m_state.blocked = blocked;
    m_state.pending = int(m_outbox.size()) - blocked;
    m_state.uploads = int(m_uploads.size());
    emit stateChanged();
}

void SyncEngine::setStatus(SyncState::Status s, const QString &error) {
    m_state.status = isLocalOnly() ? SyncState::Local : s;
    m_state.error = error;
    refreshState();
}

// --- sync loop ---

void SyncEngine::sync() {
    if (!m_transport || m_stopped || m_paused) return;
    if (m_running) {
        m_again = true;
        return;
    }
    m_running = true;
    m_again = false;
    runOnce();
}

void SyncEngine::scheduleSync(int delayMs) {
    if (!m_transport || m_stopped || m_paused) return;
    m_syncTimer.start(delayMs < 0 ? m_debounceMs : delayMs);
}

void SyncEngine::onServerSeq(qint64 seq) {
    if (seq > m_cursor) sync();
}

void SyncEngine::stop() {
    m_stopped = true;
    m_syncTimer.stop();
    m_notifyTimer.stop();
    persistNow();
}

void SyncEngine::pause() {
    m_paused = true;
    m_syncTimer.stop();
}

void SyncEngine::resume() {
    m_paused = false;
    m_retryDelay = 2000;
}

void SyncEngine::runOnce() {
    m_syncTimer.stop();
    setStatus(SyncState::Syncing);
    QPointer<SyncEngine> self(this);
    // Uploads first, so local- references in bodies are replaced before the push.
    flushUploads([self](const TransportError &e) {
        if (!self) return;
        if (!e.ok) return self->finishRound(e);
        self->push([self](const TransportError &e) {
            if (!self) return;
            if (!e.ok) return self->finishRound(e);
            self->pull([self](const TransportError &e) {
                if (self) self->finishRound(e);
            });
        });
    });
}

void SyncEngine::finishRound(const TransportError &err) {
    m_running = false;
    if (m_stopped) return;
    if (err.ok) {
        m_retryDelay = 2000;
        m_state.lastSyncAt = now();
        m_store->setMeta(QStringLiteral("lastSyncAt"), QString::number(m_state.lastSyncAt));
        setStatus(SyncState::Idle);
        emit roundFinished(true);
        if (m_again) sync();
        return;
    }
    m_again = false;
    if (err.status == 401) {
        // Keep the data and the outbox; pushing resumes after signing in again.
        m_paused = true;
        setStatus(SyncState::Error, QStringLiteral("unauthorized"));
        emit roundFinished(false);
        emit unauthorized();
        return;
    }
    qCInfo(lcSync) << "sync failed, status" << err.status;
    setStatus(err.network() ? SyncState::Offline : SyncState::Error, err.network() ? QString() : err.message);
    emit roundFinished(false);
    scheduleSync(m_retryDelay);
    m_retryDelay = std::min(m_retryDelay * 2, 60000);
}

// --- uploads ---

void SyncEngine::flushUploads(Done done) {
    flushNextUpload(std::move(done));
}

void SyncEngine::flushNextUpload(Done done) {
    if (m_stopped) return done(TransportError::success());
    // Pick the oldest pending non-failed upload dynamically so files added
    // while an earlier upload is in flight are also processed cleanly.
    QString localId;
    qint64 oldest = 0;
    for (const auto &u : std::as_const(m_uploads)) {
        if (!u.failed.isEmpty()) continue;
        if (localId.isEmpty() || u.createdAt < oldest || (u.createdAt == oldest && u.id < localId)) {
            localId = u.id;
            oldest = u.createdAt;
        }
    }
    if (localId.isEmpty()) return done(TransportError::success());
    const PendingUpload up = m_uploads.value(localId);
    QByteArray data;
    if (!m_files->readUpload(localId, &data)) {
        // The file is gone from disk; nothing will ever upload it.
        m_uploads[localId].failed = QStringLiteral("missing file");
        emit uploadFailed(up.name, QStringLiteral("missing file"));
        return flushNextUpload(std::move(done));
    }
    QPointer<SyncEngine> self(this);
    m_transport->upload(data, up.name, up.mime, [self, localId, data, done](TransportError e, Attachment att) {
        if (!self) return;
        if (!e.ok) {
            if (!refused(e)) return done(e);
            // One bad file must not hold back everything else.
            auto it = self->m_uploads.find(localId);
            if (it != self->m_uploads.end()) {
                it->failed = e.message;
                emit self->uploadFailed(it->name, e.message);
            }
            return self->flushNextUpload(done);
        }
        emit self->aboutToApplyRemote();
        self->m_attachments.insert(att.id, att);
        self->m_dirty.attachments.insert(att.id);
        // Promote local copy directly into the synced attachment cache (zero re-download)
        // and register runtime alias so in-flight loads keep resolving cleanly.
        self->m_files->promoteUpload(localId, att.id, data);
        const QString from = QStringLiteral("ivy://res/") + localId;
        const QString to = QStringLiteral("ivy://res/") + att.id;
        QStringList touched;
        for (const auto &n : std::as_const(self->m_notes))
            if (n.body.contains(from)) touched << n.id;
        for (const QString &id : touched) {
            QString body = self->m_notes.value(id).body;
            body.replace(from, to);
            NotePatch p;
            p.body = body;
            self->updateNote(id, p);
        }
        // Notify open editors & sessions so links normalize in place without
        // stealing focus, jumping the cursor, or clobbering concurrent typing.
        emit self->resourceNormalized(localId, att.id);
        // Commit normalized bodies before removing the staging upload.
        self->persistNow();
        self->m_uploads.remove(localId);
        self->m_dirty.uploads.insert(localId);
        self->m_files->removeUpload(localId);
        self->persistNow();
        self->refreshState();
        self->flushNextUpload(done);
    });
}

// --- push ---

void SyncEngine::push(Done done) {
    QList<OutboxEntry> entries;
    for (const auto &e : std::as_const(m_outbox))
        if (e.blocked.isEmpty()) entries << e;
    // In the order they were first queued, so a notebook goes out before the
    // notes filed into it and a parent before its children.
    std::sort(entries.begin(), entries.end(), [](const OutboxEntry &a, const OutboxEntry &b) { return a.queuedAt < b.queuedAt; });
    QStringList keys;
    for (const auto &e : entries) keys << e.key;
    pushNext(keys, 0, std::move(done));
}

void SyncEngine::pushNext(QStringList keys, int index, Done done) {
    QList<OutboxEntry> batch;
    qsizetype bytes = 0;
    int i = index;
    for (; i < keys.size() && batch.size() < kPushBatch; ++i) {
        auto it = m_outbox.constFind(keys[i]);
        if (it == m_outbox.constEnd() || !it->blocked.isEmpty()) continue;
        const qsizetype size = QJsonDocument(it->toChange()).toJson(QJsonDocument::Compact).size();
        if (!batch.isEmpty() && bytes + size > kPushBytes) break;
        batch << *it; // a snapshot: edits made while it is in flight bump the live version
        bytes += size;
    }
    if (batch.isEmpty()) {
        if (i >= keys.size()) return done(TransportError::success());
        return pushNext(keys, i, std::move(done));
    }
    QPointer<SyncEngine> self(this);
    sendBatch(batch, [self, keys, i, done](const TransportError &e) {
        if (!self) return;
        if (!e.ok) return done(e);
        self->persistNow();
        self->refreshState();
        self->pushNext(keys, i, done);
    });
}

void SyncEngine::sendBatch(QList<OutboxEntry> batch, Done done) {
    QList<QJsonObject> changes;
    for (const auto &e : batch) changes << e.toChange();
    QPointer<SyncEngine> self(this);
    m_transport->push(changes, [self, batch, done](TransportError e, QList<ChangeResult> results, qint64) {
        if (!self) return;
        if (!e.ok) {
            if (e.status != 400 && e.status != 413) return done(e);
            // A refused batch is split until the change at fault is alone.
            if (batch.size() == 1) {
                ChangeResult r;
                r.type = typeName(batch[0].type);
                r.id = batch[0].id;
                r.error = e.message;
                self->applyResult(batch[0], r);
                return done(TransportError::success());
            }
            const auto mid = batch.size() / 2;
            const auto first = batch.mid(0, mid);
            const auto second = batch.mid(mid);
            return self->sendBatch(first, [self, second, done](const TransportError &e) {
                if (!self) return;
                if (!e.ok) return done(e);
                self->sendBatch(second, done);
            });
        }
        emit self->aboutToApplyRemote();
        for (int i = 0; i < batch.size(); ++i) {
            ChangeResult r;
            if (i < results.size()) r = results[i];
            else r.error = QStringLiteral("missing result");
            self->applyResult(batch[i], r);
        }
        done(TransportError::success());
    });
}

void SyncEngine::applyResult(const OutboxEntry &sent, const ChangeResult &r) {
    auto cur = m_outbox.find(sent.key);
    const bool exists = cur != m_outbox.end();
    const bool unchanged = exists && cur->version == sent.version;
    if (!r.ok) {
        // The edit stays rather than vanish: it waits for the next edit or a
        // restart, and pulls keep its fields.
        qCInfo(lcSync) << "change rejected:" << typeName(sent.type) << r.error;
        emit rejected(typeName(sent.type), sent.id, r.error);
        if (unchanged) {
            cur->blocked = r.error.isEmpty() ? QStringLiteral("rejected") : r.error;
            m_dirty.outbox.insert(sent.key);
        }
        return;
    }
    if (unchanged) {
        m_outbox.erase(cur);
        m_dirty.outbox.insert(sent.key);
        cur = m_outbox.end();
    }
    const bool hasCur = cur != m_outbox.end();
    if (sent.type != EntityType::Note) {
        if (sent.type == EntityType::Folder) {
            auto f = m_folders.find(sent.id);
            if (f != m_folders.end() && r.seq) f->seq = r.seq;
        } else {
            auto t = m_tags.find(sent.id);
            if (t != m_tags.end() && r.seq) t->seq = r.seq;
        }
        return;
    }
    auto n = m_notes.find(sent.id);
    if (n == m_notes.end() || sent.isDelete) return;
    if (r.seq && r.conflictCopy.isEmpty()) n->seq = r.seq;
    const bool bodySent = sent.fields.contains(QLatin1String("body"));
    if (!r.conflictCopy.isEmpty()) {
        // The server kept its body; the pushed one lives in the copy, and so
        // does anything written since. The next pull restores the note.
        if (hasCur && cur->fields.contains(QLatin1String("body"))) {
            const QString body = cur->fields.value(QLatin1String("body")).toString();
            cur->fields.remove(QLatin1String("body"));
            cur->hasBaseRev = false;
            m_dirty.outbox.insert(sent.key);
            if (cur->fields.isEmpty() && !cur->isDelete) m_outbox.erase(cur);
            enqueue(EntityType::Note, r.conflictCopy, QJsonObject{{"body", body}}, false, 1);
        }
        emit conflict(sent.id, r.conflictCopy);
        return;
    }
    if (r.rev) {
        if (!hasCur) {
            n->rev = r.rev;
        } else if (bodySent && !r.merged) {
            // Later edits build on the accepted body.
            n->rev = r.rev;
            cur->baseRev = r.rev;
            cur->hasBaseRev = true;
            m_dirty.outbox.insert(sent.key);
        } else if (!bodySent) {
            n->rev = r.rev;
        }
        // Merged while newer local edits wait: keep the old base so the
        // server merges the newer text against it again.
    }
    if (r.merged && r.hasBody && !hasCur && n->body != r.body) {
        n->body = r.body;
        n->words = md::wordCount(r.body);
        emit remoteBody(n->id, r.body);
    }
    m_dirty.notes.insert(n->id);
    bump();
}

// --- pull ---

void SyncEngine::pull(Done done) {
    const bool snapshot = m_cursor == 0;
    QPointer<SyncEngine> self(this);
    auto handle = [self, snapshot, done](TransportError e, PullResponse res) {
        if (!self) return;
        if (!e.ok) return done(e);
        self->applyPull(res, snapshot);
        self->m_cursor = res.seq;
        // Data and cursor land in one transaction: the cursor never runs ahead.
        self->persistNow();
        done(TransportError::success());
    };
    if (snapshot) m_transport->snapshot(handle);
    else m_transport->pull(m_cursor, handle);
}

template <typename T>
void SyncEngine::applyRemote(EntityType type, const T &remote, QHash<QString, T> &map, QSet<QString> &dirty) {
    const QString key = keyOf(type, remote.id);
    auto pending = m_outbox.find(key);
    if (remote.deleted) {
        map.remove(remote.id);
        dirty.insert(remote.id);
        if (pending != m_outbox.end()) {
            // A tombstone beats unpushed edits.
            m_outbox.erase(pending);
            m_dirty.outbox.insert(key);
        }
        return;
    }
    if (pending != m_outbox.end() && pending->isDelete) return; // our delete waits to be pushed
    T merged = remote;
    auto local = map.constFind(remote.id);
    if (pending != m_outbox.end()) {
        if (local != map.constEnd()) {
            // Fields waiting to be pushed keep their local values.
            if constexpr (std::is_same_v<T, Note>) {
                for (auto it = pending->fields.begin(); it != pending->fields.end(); ++it) {
                    const QString &f = it.key();
                    if (f == QLatin1String("trashed")) merged.trashedAt = local->trashedAt;
                    else if (f == QLatin1String("title")) merged.title = local->title;
                    else if (f == QLatin1String("body")) { merged.body = local->body; merged.words = local->words; }
                    else if (f == QLatin1String("folderId")) merged.folderId = local->folderId;
                    else if (f == QLatin1String("tags")) merged.tags = local->tags;
                    else if (f == QLatin1String("pinned")) merged.pinned = local->pinned;
                }
            } else if constexpr (std::is_same_v<T, Folder>) {
                for (auto it = pending->fields.begin(); it != pending->fields.end(); ++it) {
                    if (it.key() == QLatin1String("name")) merged.name = local->name;
                    else if (it.key() == QLatin1String("parentId")) merged.parentId = local->parentId;
                    else if (it.key() == QLatin1String("sort")) merged.sort = local->sort;
                }
            } else {
                for (auto it = pending->fields.begin(); it != pending->fields.end(); ++it) {
                    if (it.key() == QLatin1String("name")) merged.name = local->name;
                    else if (it.key() == QLatin1String("color")) merged.color = local->color;
                }
            }
        } else {
            // Written before the entity arrived here, as in a conflict copy.
            if constexpr (std::is_same_v<T, Note>) {
                const auto &f = pending->fields;
                if (f.contains(QLatin1String("title"))) merged.title = f.value(QLatin1String("title")).toString();
                if (f.contains(QLatin1String("body"))) {
                    merged.body = f.value(QLatin1String("body")).toString();
                    merged.words = md::wordCount(merged.body);
                }
                if (f.contains(QLatin1String("folderId"))) merged.folderId = f.value(QLatin1String("folderId")).toString();
                if (f.contains(QLatin1String("pinned"))) merged.pinned = f.value(QLatin1String("pinned")).toBool();
                if (f.contains(QLatin1String("tags"))) {
                    merged.tags.clear();
                    for (const auto &v : f.value(QLatin1String("tags")).toArray()) merged.tags << v.toString();
                }
            }
        }
    }
    map.insert(remote.id, merged);
    dirty.insert(remote.id);
}

void SyncEngine::applyPull(const PullResponse &res, bool snapshot) {
    emit aboutToApplyRemote();
    if (snapshot) {
        // A snapshot replaces what it does not mention, unless edits wait for it.
        auto keep = [this](auto &map, EntityType type, const QSet<QString> &present, QSet<QString> &dirty) {
            for (auto it = map.begin(); it != map.end();) {
                if (!present.contains(it.key()) && !m_outbox.contains(keyOf(type, it.key()))) {
                    dirty.insert(it.key());
                    it = map.erase(it);
                } else {
                    ++it;
                }
            }
        };
        QSet<QString> n, f, t;
        for (const auto &x : res.notes) n.insert(x.id);
        for (const auto &x : res.folders) f.insert(x.id);
        for (const auto &x : res.tags) t.insert(x.id);
        keep(m_notes, EntityType::Note, n, m_dirty.notes);
        keep(m_folders, EntityType::Folder, f, m_dirty.folders);
        keep(m_tags, EntityType::Tag, t, m_dirty.tags);
        for (auto it = m_attachments.begin(); it != m_attachments.end(); ++it) m_dirty.attachments.insert(it.key());
        m_attachments.clear();
    }
    for (const auto &f : res.folders) {
        for (const auto &ts : f.hlc) m_hlc.observe(ts);
        applyRemote(EntityType::Folder, f, m_folders, m_dirty.folders);
    }
    for (const auto &t : res.tags) {
        for (const auto &ts : t.hlc) m_hlc.observe(ts);
        applyRemote(EntityType::Tag, t, m_tags, m_dirty.tags);
    }
    for (const auto &n : res.notes) {
        for (const auto &ts : n.hlc) m_hlc.observe(ts);
        const auto before = m_notes.constFind(n.id);
        const QString oldBody = before != m_notes.constEnd() ? before->body : QString();
        const bool existed = before != m_notes.constEnd();
        applyRemote(EntityType::Note, n, m_notes, m_dirty.notes);
        const auto after = m_notes.constFind(n.id);
        if (existed && after != m_notes.constEnd() && after->body != oldBody) emit remoteBody(n.id, after->body);
    }
    for (const auto &a : res.attachments) {
        m_dirty.attachments.insert(a.id);
        if (a.deleted) {
            m_attachments.remove(a.id);
            m_files->removeFile(a.id);
        } else {
            m_attachments.insert(a.id, a);
        }
    }
    bump(true);
}

// --- local mutations ---

void SyncEngine::enqueue(EntityType type, const QString &id, const QJsonObject &fields, bool isDelete,
                         std::optional<qint64> baseRev) {
    if (isLocalOnly()) {
        schedulePersist();
        return;
    }
    const QString key = keyOf(type, id);
    const QString ts = m_hlc.tick();
    auto it = m_outbox.find(key);
    if (it == m_outbox.end()) {
        OutboxEntry e;
        e.key = key;
        e.type = type;
        e.id = id;
        e.device = m_device;
        e.queuedAt = ts;
        it = m_outbox.insert(key, e);
    }
    if (isDelete) {
        it->isDelete = true;
        it->fields = QJsonObject();
        it->hasBaseRev = false;
    } else {
        for (auto f = fields.begin(); f != fields.end(); ++f) it->fields.insert(f.key(), f.value());
        if (fields.contains(QLatin1String("body")) && !it->hasBaseRev && baseRev) {
            it->hasBaseRev = true;
            it->baseRev = *baseRev;
        }
    }
    it->blocked.clear();
    it->hlc = ts;
    it->version++;
    m_dirty.outbox.insert(key);
    schedulePersist();
    refreshState();
    scheduleSync();
}

void SyncEngine::schedulePersist() {
    if (!m_persistTimer.isActive()) m_persistTimer.start(m_persistMs);
}

bool SyncEngine::persistNow() {
    m_persistTimer.stop();
    const bool cursorMoved = m_savedCursor != m_cursor;
    if (!m_dirty.any() && !cursorMoved) return true;
    const Dirty d = m_dirty;
    m_dirty = Dirty();
    StoreBatch b;
    auto split = [&b](const QSet<QString> &ids, const auto &map, Table table) {
        for (const QString &id : ids) {
            auto it = map.constFind(id);
            if (it != map.constEnd()) b.puts.append({table, id, it->toJson()});
            else b.dels.append({table, id});
        }
    };
    split(d.notes, m_notes, Table::Notes);
    split(d.folders, m_folders, Table::Folders);
    split(d.tags, m_tags, Table::Tags);
    split(d.attachments, m_attachments, Table::Attachments);
    split(d.outbox, m_outbox, Table::Outbox);
    split(d.uploads, m_uploads, Table::Uploads);
    if (cursorMoved) b.meta.insert(QStringLiteral("cursor"), QString::number(m_cursor));
    const qint64 cursor = m_cursor;
    if (!m_store->apply(b)) {
        // Keep everything dirty and try again shortly.
        m_dirty.notes.unite(d.notes);
        m_dirty.folders.unite(d.folders);
        m_dirty.tags.unite(d.tags);
        m_dirty.attachments.unite(d.attachments);
        m_dirty.outbox.unite(d.outbox);
        m_dirty.uploads.unite(d.uploads);
        schedulePersist();
        return false;
    }
    m_savedCursor = cursor;
    return true;
}

const Note *SyncEngine::note(const QString &id) const {
    auto it = m_notes.constFind(id);
    return it == m_notes.constEnd() ? nullptr : &*it;
}

bool SyncEngine::hasPending(EntityType type, const QString &id) const {
    return m_outbox.contains(keyOf(type, id));
}

QList<const Note *> SyncEngine::noteList() const {
    QList<const Note *> out;
    out.reserve(m_notes.size());
    for (const auto &n : m_notes) out << &n;
    return out;
}

QList<const Folder *> SyncEngine::folderList() const {
    QList<const Folder *> out;
    for (const auto &f : m_folders) out << &f;
    return out;
}

QList<const Tag *> SyncEngine::tagList() const {
    QList<const Tag *> out;
    for (const auto &t : m_tags) out << &t;
    return out;
}

QList<const Folder *> SyncEngine::folderPath(const QString &folderId) const {
    QList<const Folder *> path;
    QString id = folderId;
    for (int d = 0; !id.isEmpty() && d < 64; ++d) {
        auto it = m_folders.constFind(id);
        if (it == m_folders.constEnd()) break;
        path.prepend(&*it);
        id = it->parentId;
    }
    return path;
}

Note SyncEngine::createNote(const NoteInit &init) {
    const qint64 t = now();
    Note n;
    n.id = ids::valid(init.id) && !m_notes.contains(init.id) ? init.id : ids::newId();
    n.folderId = init.folderId;
    n.title = init.title;
    n.body = init.body;
    n.tags = init.tags;
    n.pinned = init.pinned;
    n.words = md::wordCount(init.body);
    n.createdAt = init.createdAt > 0 ? init.createdAt : t;
    n.updatedAt = t;
    m_notes.insert(n.id, n);
    m_dirty.notes.insert(n.id);
    QJsonObject fields{{"title", n.title}, {"body", n.body}, {"createdAt", double(n.createdAt)}};
    if (!n.folderId.isEmpty()) fields.insert(QStringLiteral("folderId"), n.folderId);
    if (!n.tags.isEmpty()) fields.insert(QStringLiteral("tags"), toJsonValue(n.tags));
    if (n.pinned) fields.insert(QStringLiteral("pinned"), true);
    enqueue(EntityType::Note, n.id, fields, false, 0);
    bump(true);
    return n;
}

void SyncEngine::updateNote(const QString &id, const NotePatch &p) {
    auto n = m_notes.find(id);
    if (n == m_notes.end()) return;
    QJsonObject fields;
    if (p.title && *p.title != n->title) {
        n->title = *p.title;
        fields.insert(QStringLiteral("title"), n->title);
    }
    if (p.body && *p.body != n->body) {
        n->body = *p.body;
        n->words = md::wordCount(n->body);
        fields.insert(QStringLiteral("body"), n->body);
    }
    if (p.folderId && *p.folderId != n->folderId) {
        n->folderId = *p.folderId;
        fields.insert(QStringLiteral("folderId"), n->folderId);
    }
    if (p.tags && *p.tags != n->tags) {
        n->tags = *p.tags;
        fields.insert(QStringLiteral("tags"), toJsonValue(n->tags));
    }
    if (p.pinned && *p.pinned != n->pinned) {
        n->pinned = *p.pinned;
        fields.insert(QStringLiteral("pinned"), n->pinned);
    }
    if (p.trashed && *p.trashed != n->trashed()) {
        n->trashedAt = *p.trashed ? now() : 0;
        fields.insert(QStringLiteral("trashed"), *p.trashed);
    }
    if (fields.isEmpty()) return;
    n->updatedAt = now();
    m_dirty.notes.insert(id);
    const qint64 rev = n->rev;
    enqueue(EntityType::Note, id, fields, false, rev);
    const bool structural = fields.contains(QLatin1String("trashed")) || fields.contains(QLatin1String("folderId")) ||
                            fields.contains(QLatin1String("pinned")) || fields.contains(QLatin1String("tags"));
    bump(structural);
}

void SyncEngine::deleteNote(const QString &id) {
    if (!m_notes.remove(id)) return;
    m_dirty.notes.insert(id);
    enqueue(EntityType::Note, id, {}, true);
    bump(true);
}

Folder SyncEngine::createFolder(const QString &name, const QString &parentId) {
    const qint64 t = now();
    double sort = 0;
    for (const auto &f : std::as_const(m_folders))
        if (f.parentId == parentId) sort = std::max(sort, f.sort);
    Folder f;
    f.id = ids::newId();
    f.parentId = parentId;
    f.name = name.trimmed();
    f.sort = sort + 1;
    f.createdAt = t;
    f.updatedAt = t;
    m_folders.insert(f.id, f);
    m_dirty.folders.insert(f.id);
    enqueue(EntityType::Folder, f.id, QJsonObject{{"name", f.name}, {"parentId", parentId}, {"sort", f.sort}});
    bump(true);
    return f;
}

void SyncEngine::updateFolder(const QString &id, const std::optional<QString> &name, const std::optional<QString> &parentId,
                              const std::optional<double> &sort) {
    auto f = m_folders.find(id);
    if (f == m_folders.end()) return;
    if (parentId && !canMoveFolder(id, *parentId)) return;
    QJsonObject fields;
    if (name && name->trimmed() != f->name) {
        f->name = name->trimmed();
        fields.insert(QStringLiteral("name"), f->name);
    }
    if (parentId && *parentId != f->parentId) {
        f->parentId = *parentId;
        fields.insert(QStringLiteral("parentId"), f->parentId);
    }
    if (sort && *sort != f->sort) {
        f->sort = *sort;
        fields.insert(QStringLiteral("sort"), f->sort);
    }
    if (fields.isEmpty()) return;
    f->updatedAt = now();
    m_dirty.folders.insert(id);
    enqueue(EntityType::Folder, id, fields);
    bump(true);
}

bool SyncEngine::canMoveFolder(const QString &id, const QString &parent) const {
    QString p = parent;
    for (int depth = 0; !p.isEmpty(); ++depth) {
        if (p == id || depth > 64) return false;
        auto f = m_folders.constFind(p);
        if (f == m_folders.constEnd()) return false;
        p = f->parentId;
    }
    return true;
}

void SyncEngine::deleteFolder(const QString &id) {
    if (!m_folders.contains(id)) return;
    QSet<QString> doomed{id};
    for (bool grew = true; grew;) {
        grew = false;
        for (const auto &f : std::as_const(m_folders)) {
            if (!doomed.contains(f.id) && doomed.contains(f.parentId)) {
                doomed.insert(f.id);
                grew = true;
            }
        }
    }
    const qint64 t = now();
    for (auto &n : m_notes) {
        if (!doomed.contains(n.folderId)) continue;
        n.folderId.clear();
        if (!n.trashedAt) n.trashedAt = t;
        m_dirty.notes.insert(n.id);
    }
    for (const QString &fid : doomed) {
        m_folders.remove(fid);
        m_dirty.folders.insert(fid);
    }
    // Only the root goes out: the server cascades like we just did.
    enqueue(EntityType::Folder, id, {}, true);
    bump(true);
}

Tag SyncEngine::createTag(const QString &name, const QString &color) {
    const qint64 t = now();
    Tag tag;
    tag.id = ids::newId();
    tag.name = name.trimmed();
    while (tag.name.startsWith(u'#')) tag.name = tag.name.mid(1).trimmed();
    tag.color = color;
    tag.createdAt = t;
    tag.updatedAt = t;
    m_tags.insert(tag.id, tag);
    m_dirty.tags.insert(tag.id);
    enqueue(EntityType::Tag, tag.id, QJsonObject{{"name", tag.name}, {"color", color}});
    bump(true);
    return tag;
}

void SyncEngine::updateTag(const QString &id, const std::optional<QString> &name, const std::optional<QString> &color) {
    auto t = m_tags.find(id);
    if (t == m_tags.end()) return;
    QJsonObject fields;
    if (name) {
        QString n = name->trimmed();
        while (n.startsWith(u'#')) n = n.mid(1).trimmed();
        if (!n.isEmpty() && n != t->name) {
            t->name = n;
            fields.insert(QStringLiteral("name"), n);
        }
    }
    if (color && *color != t->color) {
        t->color = *color;
        fields.insert(QStringLiteral("color"), *color);
    }
    if (fields.isEmpty()) return;
    t->updatedAt = now();
    m_dirty.tags.insert(id);
    enqueue(EntityType::Tag, id, fields);
    bump(true);
}

void SyncEngine::deleteTag(const QString &id) {
    if (!m_tags.remove(id)) return;
    m_dirty.tags.insert(id);
    for (auto &n : m_notes) {
        if (n.tags.removeAll(id) > 0) m_dirty.notes.insert(n.id);
    }
    enqueue(EntityType::Tag, id, {}, true);
    bump(true);
}

// --- attachments ---

void SyncEngine::addFile(const QByteArray &data, const QString &name, const QString &mime, std::function<void(AddedFile)> done) {
    const QString kind = kindOfMime(mime);
    QPointer<SyncEngine> self(this);
    if (isLocalOnly()) {
        // Local vaults keep files themselves, deduplicated by content like the server.
        const QString sha = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
        for (const auto &a : std::as_const(m_attachments)) {
            if (a.sha256 == sha && !a.deleted && m_files->hasFile(a.id)) {
                QTimer::singleShot(0, this, [a, done] { done({true, a.id, a.kind, a.name, false, {}}); });
                return;
            }
        }
        Attachment a;
        a.id = ids::newId();
        a.name = name;
        a.mime = mime;
        a.kind = kind;
        a.size = data.size();
        a.sha256 = sha;
        a.storage = QStringLiteral("local");
        a.createdAt = a.updatedAt = now();
        if (kind == QLatin1String("image")) {
            QBuffer buf;
            buf.setData(data);
            QImageReader reader(&buf);
            const QSize s = reader.size();
            a.width = s.width();
            a.height = s.height();
        }
        if (!m_files->putFile(a.id, data)) {
            QTimer::singleShot(0, this, [done] { done({false, {}, {}, {}, false, QStringLiteral("write failed")}); });
            return;
        }
        m_attachments.insert(a.id, a);
        m_dirty.attachments.insert(a.id);
        persistNow();
        bump(true);
        QTimer::singleShot(0, this, [a, done] { done({true, a.id, a.kind, a.name, false, {}}); });
        return;
    }
    // Local-first staging for synced vaults: copy directly into the local
    // attachment library so the image displays immediately with zero network wait,
    // then upload gradually in the background and normalize links once synced.
    const QString sha = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
    for (const auto &u : std::as_const(m_uploads)) {
        if (u.failed.isEmpty() && !u.sha256.isEmpty() && u.sha256 == sha && m_files->hasUpload(u.id)) {
            if (!m_stopped && !m_paused) scheduleSync(std::min(m_debounceMs, 250));
            QTimer::singleShot(0, this, [id = u.id, kind, name, done] { done({true, id, kind, name, true, {}}); });
            return;
        }
    }
    PendingUpload up;
    up.id = QStringLiteral("local-") + ids::newId();
    while (m_uploads.contains(up.id)) up.id = QStringLiteral("local-") + ids::newId();
    up.name = name;
    up.mime = mime;
    up.sha256 = sha;
    up.createdAt = now();
    if (!m_files->putUpload(up.id, data)) {
        QTimer::singleShot(0, this, [done] { done({false, {}, {}, {}, false, QStringLiteral("write failed")}); });
        return;
    }
    m_uploads.insert(up.id, up);
    m_dirty.uploads.insert(up.id);
    persistNow();
    refreshState();
    if (!m_stopped && !m_paused) scheduleSync(std::min(m_debounceMs, 250));
    QTimer::singleShot(0, this, [id = up.id, kind, name, done] { done({true, id, kind, name, true, {}}); });
}

QString SyncEngine::attachmentKind(const QString &id) const {
    if (id.startsWith(QLatin1String("local-"))) {
        auto u = m_uploads.constFind(id);
        if (u != m_uploads.constEnd()) return kindOfMime(u->mime);
    }
    const QString cid = m_files ? m_files->canonicalId(id) : id;
    auto a = m_attachments.constFind(cid);
    return a == m_attachments.constEnd() ? QString() : a->kind;
}

QString SyncEngine::attachmentName(const QString &id) const {
    if (id.startsWith(QLatin1String("local-"))) {
        auto u = m_uploads.constFind(id);
        if (u != m_uploads.constEnd()) return u->name;
    }
    const QString cid = m_files ? m_files->canonicalId(id) : id;
    return m_attachments.value(cid).name;
}

void SyncEngine::reset() {
    m_syncTimer.stop();
    m_persistTimer.stop();
    m_notes.clear();
    m_folders.clear();
    m_tags.clear();
    m_attachments.clear();
    m_outbox.clear();
    m_uploads.clear();
    m_dirty = Dirty();
    m_cursor = m_savedCursor = 0;
    m_store->clear();
    m_files->clearAll();
    m_state.lastSyncAt = 0;
    refreshState();
    bump(true);
}

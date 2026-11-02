// The engine against a scripted transport: outbox, results, pulls.
#include "storage/localstore.h"
#include "sync/engine.h"
#include "sync/transport.h"
#include "vault/files.h"

#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <deque>

namespace {

// Answers each call with the next scripted reply (or a default).
struct FakeTransport : Transport {
    struct PushCall {
        QList<QJsonObject> changes;
    };
    QList<PushCall> pushes;
    std::deque<std::function<void(const QList<QJsonObject> &, std::function<void(TransportError, QList<ChangeResult>, qint64)>)>> pushScript;
    std::deque<PullResponse> pulls;
    int snapshots = 0;
    qint64 seq = 1;
    bool offline = false;
    // Runs while a push is "in flight", before its answer arrives.
    std::function<void()> duringPush;

    static ChangeResult okFor(const QJsonObject &c, qint64 seq, qint64 rev = 0) {
        ChangeResult r;
        r.type = c.value("type").toString();
        r.id = c.value("id").toString();
        r.ok = true;
        r.seq = seq;
        r.rev = rev;
        return r;
    }

    void snapshot(std::function<void(TransportError, PullResponse)> done) override {
        snapshots++;
        pull(0, std::move(done));
    }
    void pull(qint64, std::function<void(TransportError, PullResponse)> done) override {
        QTimer::singleShot(0, [this, done] {
            if (offline) return done(TransportError::failure(0, "offline"), {});
            PullResponse p;
            p.seq = seq;
            if (!pulls.empty()) {
                p = pulls.front();
                pulls.pop_front();
                seq = std::max(seq, p.seq);
            }
            done(TransportError::success(), p);
        });
    }
    void push(const QList<QJsonObject> &changes, std::function<void(TransportError, QList<ChangeResult>, qint64)> done) override {
        pushes.append({changes});
        if (duringPush) {
            auto f = duringPush;
            duringPush = nullptr;
            f();
        }
        QTimer::singleShot(0, [this, changes, done] {
            if (offline) return done(TransportError::failure(0, "offline"), {}, 0);
            if (!pushScript.empty()) {
                auto f = pushScript.front();
                pushScript.pop_front();
                return f(changes, done);
            }
            QList<ChangeResult> rs;
            for (const auto &c : changes) rs << okFor(c, ++seq, c.value("fields").toObject().contains("body") ? 1 : 0);
            done(TransportError::success(), rs, seq);
        });
    }
    void upload(const QByteArray &, const QString &name, const QString &mime, std::function<void(TransportError, Attachment)> done) override {
        QTimer::singleShot(0, [this, name, mime, done] {
            if (offline) return done(TransportError::failure(0, "offline"), {});
            Attachment a;
            a.id = "att0000000000001";
            a.name = name;
            a.mime = mime;
            a.kind = kindOfMime(mime);
            done(TransportError::success(), a);
        });
    }
};

struct Rig {
    QTemporaryDir dir;
    std::unique_ptr<LocalStore> store;
    std::unique_ptr<VaultFiles> files;
    FakeTransport transport;
    std::unique_ptr<SyncEngine> engine;

    // A rig starts as an already synced device (cursor 1) unless fresh.
    explicit Rig(bool fresh = false) {
        store = std::make_unique<LocalStore>(dir.filePath("v.db"), VaultCipher());
        store->open();
        if (!fresh) store->setMeta("cursor", "1");
        open();
    }
    void open() {
        engine.reset();
        store = std::make_unique<LocalStore>(dir.filePath("v.db"), VaultCipher());
        store->open();
        files = std::make_unique<VaultFiles>(dir.path(), VaultCipher());
        engine = std::make_unique<SyncEngine>(store.get(), files.get(), &transport, "test-dev");
        engine->setOptions({60000, 10, {}}); // no automatic syncs during tests
        engine->init();
    }
    bool syncAndWait() {
        QSignalSpy spy(engine.get(), &SyncEngine::roundFinished);
        engine->sync();
        if (!spy.wait(3000)) return false;
        return spy.last().at(0).toBool();
    }
};

// A push answer refusing every change, so it stays in the outbox.
auto hold() {
    return [](const QList<QJsonObject> &c, std::function<void(TransportError, QList<ChangeResult>, qint64)> done) {
        QList<ChangeResult> rs;
        for (const auto &x : c) {
            ChangeResult r;
            r.type = x.value("type").toString();
            r.id = x.value("id").toString();
            r.error = "hold";
            rs << r;
        }
        done(TransportError::success(), rs, 0);
    };
}

} // namespace

class EngineTest : public QObject {
    Q_OBJECT
private slots:
    void outboxSurvivesRestart() {
        Rig rig;
        rig.transport.offline = true;
        const Folder f = rig.engine->createFolder("Work");
        const Note n = rig.engine->createNote({.title = "T", .body = "B", .folderId = f.id});
        QVERIFY(!rig.syncAndWait());
        QCOMPARE(rig.engine->state().status, SyncState::Offline);
        rig.open(); // restart
        QCOMPARE(rig.engine->outbox().size(), 2);
        QCOMPARE(rig.engine->notes().value(n.id).title, QStringLiteral("T"));
        rig.transport.offline = false;
        QVERIFY(rig.syncAndWait());
        QCOMPARE(rig.engine->outbox().size(), 0);
        // The notebook goes out before the note filed into it.
        const auto changes = rig.transport.pushes.last().changes;
        QCOMPARE(changes[0].value("type").toString(), QStringLiteral("folder"));
        QCOMPARE(changes[1].value("fields").toObject().value("baseRev").toInt(), 0);
    }

    void editDuringPushKeepsNewText() {
        Rig rig;
        const Note n = rig.engine->createNote({.title = "T", .body = "v1"});
        QVERIFY(rig.syncAndWait());
        QCOMPARE(rig.engine->notes().value(n.id).rev, 1);
        rig.engine->updateNote(n.id, {.body = QString("v2")});
        rig.transport.duringPush = [&] { rig.engine->updateNote(n.id, {.body = QString("v3")}); };
        rig.transport.pushScript.push_back([](const QList<QJsonObject> &c, auto done) {
            done(TransportError::success(), {FakeTransport::okFor(c[0], 10, 2)}, 10);
        });
        rig.engine->sync();
        QTRY_VERIFY(!rig.engine->running());
        // v3 is still queued and now builds on the accepted rev 2.
        const auto e = rig.engine->outbox().value("note:" + n.id);
        QCOMPARE(e.fields.value("body").toString(), QStringLiteral("v3"));
        QCOMPARE(e.baseRev, 2);
        QCOMPARE(rig.engine->notes().value(n.id).body, QStringLiteral("v3"));
    }

    void mergedBodyAdopted() {
        Rig rig;
        const Note n = rig.engine->createNote({.body = "a"});
        QVERIFY(rig.syncAndWait());
        rig.engine->updateNote(n.id, {.body = QString("a\nmine")});
        QSignalSpy remote(rig.engine.get(), &SyncEngine::remoteBody);
        rig.transport.pushScript.push_back([](const QList<QJsonObject> &c, auto done) {
            auto r = FakeTransport::okFor(c[0], 11, 3);
            r.merged = true;
            r.hasBody = true;
            r.body = "theirs\na\nmine";
            done(TransportError::success(), {r}, 11);
        });
        QVERIFY(rig.syncAndWait());
        QCOMPARE(rig.engine->notes().value(n.id).body, QStringLiteral("theirs\na\nmine"));
        QCOMPARE(rig.engine->notes().value(n.id).rev, 3);
        QCOMPARE(remote.size(), 1);
    }

    void conflictMovesNewTypingToCopy() {
        Rig rig;
        const Note n = rig.engine->createNote({.body = "base"});
        QVERIFY(rig.syncAndWait());
        rig.engine->updateNote(n.id, {.body = QString("mine")});
        rig.transport.duringPush = [&] { rig.engine->updateNote(n.id, {.body = QString("mine + more"), .pinned = true}); };
        rig.transport.pushScript.push_back([](const QList<QJsonObject> &c, auto done) {
            auto r = FakeTransport::okFor(c[0], 12, 2);
            r.conflictCopy = "copy000000000001";
            done(TransportError::success(), {r}, 12);
        });
        QSignalSpy conflict(rig.engine.get(), &SyncEngine::conflict);
        rig.engine->sync();
        QTRY_VERIFY(!rig.engine->running());
        QCOMPARE(conflict.size(), 1);
        const auto orig = rig.engine->outbox().value("note:" + n.id);
        QVERIFY(!orig.fields.contains("body")); // the body went to the copy
        QVERIFY(orig.fields.contains("pinned"));
        const auto copy = rig.engine->outbox().value("note:copy000000000001");
        QCOMPARE(copy.fields.value("body").toString(), QStringLiteral("mine + more"));
        QCOMPARE(copy.baseRev, 1);
    }

    void rejectedStaysBlockedUntilEdited() {
        Rig rig;
        const Note n = rig.engine->createNote({.title = "x"});
        rig.transport.pushScript.push_back([](const QList<QJsonObject> &c, auto done) {
            ChangeResult r;
            r.type = "note";
            r.id = c[0].value("id").toString();
            r.error = "invalid hlc";
            done(TransportError::success(), {r}, 0);
        });
        QSignalSpy rejected(rig.engine.get(), &SyncEngine::rejected);
        QVERIFY(rig.syncAndWait());
        QCOMPARE(rejected.size(), 1);
        QCOMPARE(rig.engine->state().blocked, 1);
        const int calls = rig.transport.pushes.size();
        QVERIFY(rig.syncAndWait());
        QCOMPARE(rig.transport.pushes.size(), calls); // not sent again
        rig.engine->updateNote(n.id, {.title = QString("y")});
        QCOMPARE(rig.engine->state().blocked, 0);
        QVERIFY(rig.syncAndWait());
        QCOMPARE(rig.engine->outbox().size(), 0);
    }

    void refusedBatchIsSplit() {
        Rig rig;
        for (int i = 0; i < 4; ++i) rig.engine->createNote({.title = QString::number(i)});
        // The whole batch and its left half fail with 400; the bad change is #1.
        rig.transport.pushScript.push_back([](auto, auto done) { done(TransportError::failure(400, "bad"), {}, 0); });
        rig.transport.pushScript.push_back([](auto, auto done) { done(TransportError::failure(400, "bad"), {}, 0); });
        rig.transport.pushScript.push_back([](const QList<QJsonObject> &c, auto done) {
            done(TransportError::success(), {FakeTransport::okFor(c[0], 1)}, 1);
        });
        rig.transport.pushScript.push_back([](auto, auto done) { done(TransportError::failure(400, "too big"), {}, 0); });
        QVERIFY(rig.syncAndWait());
        QCOMPARE(rig.engine->state().blocked, 1);
        QCOMPARE(rig.engine->outbox().size(), 1);
    }

    void pullKeepsPendingFieldsAndHonorsTombstones() {
        Rig rig;
        const Note n = rig.engine->createNote({.title = "local title", .body = "body"});
        QVERIFY(rig.syncAndWait());
        rig.engine->updateNote(n.id, {.title = QString("newer local")});
        Note remote = rig.engine->notes().value(n.id);
        remote.title = "server title";
        remote.pinned = true;
        PullResponse p;
        p.seq = 50;
        p.notes = {remote};
        rig.transport.pulls.push_back(p);
        rig.transport.pushScript.push_back(hold());
        QVERIFY(rig.syncAndWait());
        // The refused title stays local; the untouched pinned flag comes in.
        QCOMPARE(rig.engine->notes().value(n.id).title, QStringLiteral("newer local"));
        QVERIFY(rig.engine->notes().value(n.id).pinned);

        Note tomb;
        tomb.id = n.id;
        tomb.deleted = true;
        PullResponse p2;
        p2.seq = 60;
        p2.notes = {tomb};
        rig.transport.pulls.push_back(p2);
        QVERIFY(rig.syncAndWait());
        QVERIFY(!rig.engine->notes().contains(n.id));
        QVERIFY(!rig.engine->outbox().contains("note:" + n.id)); // a tombstone beats local edits
    }

    void snapshotReplacesMissing() {
        Rig rig(true);
        rig.transport.offline = true;
        const Note kept = rig.engine->createNote({.title = "unpushed"});
        StoreBatch b;
        b.puts.append({Table::Notes, "stale0000000001", QJsonObject{{"id", "stale0000000001"}, {"title", "gone"}}});
        QVERIFY(rig.store->apply(b));
        rig.engine->persistNow();
        rig.open();
        QVERIFY(rig.engine->notes().contains("stale0000000001"));
        rig.transport.offline = false;
        rig.transport.pushScript.push_back(hold());
        PullResponse snap;
        snap.seq = 7;
        rig.transport.pulls.push_back(snap);
        QVERIFY(rig.syncAndWait());
        QCOMPARE(rig.transport.snapshots, 1);
        QVERIFY(!rig.engine->notes().contains("stale0000000001"));
        QVERIFY(rig.engine->notes().contains(kept.id)); // still waiting in the outbox
        QCOMPARE(rig.engine->cursor(), 7);
        QVERIFY(rig.syncAndWait());
        QCOMPARE(rig.transport.snapshots, 1); // later rounds pull
    }

    void deleteFolderCascades() {
        Rig rig;
        const Folder a = rig.engine->createFolder("A");
        const Folder b = rig.engine->createFolder("B", a.id);
        const Note n = rig.engine->createNote({.folderId = b.id});
        QVERIFY(rig.syncAndWait());
        rig.engine->deleteFolder(a.id);
        QVERIFY(rig.engine->folders().isEmpty());
        QVERIFY(rig.engine->notes().value(n.id).trashed());
        QVERIFY(rig.engine->notes().value(n.id).folderId.isEmpty());
        QCOMPARE(rig.engine->outbox().size(), 1); // only the root's delete
        QVERIFY(!rig.engine->canMoveFolder(a.id, a.id));
    }

    void offlineUploadIsReplayed() {
        Rig rig;
        rig.transport.offline = true;
        const Note n = rig.engine->createNote({.body = ""});
        AddedFile added;
        rig.engine->addFile("PNGDATA", "pic.png", "image/png", [&](AddedFile f) { added = f; });
        QTRY_VERIFY(added.ok);
        QVERIFY(added.local);
        rig.engine->updateNote(n.id, {.body = QStringLiteral("![pic](ivy://res/%1)").arg(added.id)});
        rig.transport.offline = false;
        QVERIFY(rig.syncAndWait());
        QCOMPARE(rig.engine->notes().value(n.id).body, QStringLiteral("![pic](ivy://res/att0000000000001)"));
        QVERIFY(rig.engine->uploads().isEmpty());
        QVERIFY(rig.files->hasFile("att0000000000001"));
    }

    void localOnlyVault() {
        QTemporaryDir dir;
        LocalStore store(dir.filePath("v.db"), VaultCipher(VaultCipher::generateKey()));
        QCOMPARE(store.open(), LocalStore::OpenResult::Ok);
        VaultFiles files(dir.path(), store.cipher());
        QString noteId;
        {
            SyncEngine e(&store, &files, nullptr, "dev");
            e.init();
            QCOMPARE(e.state().status, SyncState::Local);
            noteId = e.createNote({.title = "local"}).id;
            QVERIFY(e.outbox().isEmpty());
            AddedFile f1, f2;
            e.addFile("same", "a.txt", "text/plain", [&](AddedFile f) { f1 = f; });
            QTRY_VERIFY(f1.ok);
            e.addFile("same", "b.txt", "text/plain", [&](AddedFile f) { f2 = f; });
            QTRY_VERIFY(f2.ok);
            QCOMPARE(f1.id, f2.id); // deduplicated by content
        }
        SyncEngine e(&store, &files, nullptr, "dev");
        e.init();
        QCOMPARE(e.notes().value(noteId).title, QStringLiteral("local"));
        QCOMPARE(e.attachments().size(), 1);
    }
};

QTEST_GUILESS_MAIN(EngineTest)
#include "test_engine.moc"

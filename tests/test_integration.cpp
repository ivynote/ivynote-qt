// Two devices syncing through a real Ivynote server (guide 13, "同步").
// Set IVY_SERVER_BIN to the server binary; skipped otherwise.
#include "storage/localstore.h"
#include "sync/api.h"
#include "sync/engine.h"
#include "sync/events.h"
#include "sync/transport.h"
#include "vault/files.h"

#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QtTest>

namespace {

struct Device {
    QTemporaryDir dir;
    std::unique_ptr<ApiClient> api;
    std::unique_ptr<ApiTransport> transport;
    std::unique_ptr<LocalStore> store;
    std::unique_ptr<VaultFiles> files;
    std::unique_ptr<SyncEngine> engine;

    Device(const QUrl &server, const QString &token, const QString &name) {
        api = std::make_unique<ApiClient>(server);
        api->setToken(token);
        transport = std::make_unique<ApiTransport>(api.get());
        store = std::make_unique<LocalStore>(dir.filePath("v.db"), VaultCipher(VaultCipher::generateKey()));
        store->open();
        files = std::make_unique<VaultFiles>(dir.path(), store->cipher());
        engine = std::make_unique<SyncEngine>(store.get(), files.get(), transport.get(), name);
        engine->setOptions({600000, 10, {}});
        engine->init();
    }
    bool sync() {
        QSignalSpy spy(engine.get(), &SyncEngine::roundFinished);
        engine->sync();
        if (!spy.wait(20000)) return false;
        return spy.last().at(0).toBool();
    }
    const Note *byTitle(const QString &t) const {
        for (const auto &n : engine->notes())
            if (n.title == t) return &n;
        return nullptr;
    }
};

ApiReply call(ApiClient &api, const QByteArray &method, const QString &path, const QJsonObject &body = {}) {
    ApiReply out;
    bool done = false;
    api.send(method, path, method == "GET" ? QByteArray() : QJsonDocument(body).toJson(), nullptr, [&](const ApiReply &r) {
        out = r;
        done = true;
    });
    for (int i = 0; i < 1000 && !done; ++i) QTest::qWait(10);
    return out;
}

} // namespace

class IntegrationTest : public QObject {
    Q_OBJECT
    QProcess m_server;
    QTemporaryDir m_data;
    QUrl m_url;
    QString m_token;

private slots:
    void initTestCase() {
        const QString bin = qEnvironmentVariable("IVY_SERVER_BIN");
        if (bin.isEmpty()) QSKIP("IVY_SERVER_BIN not set");
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost));
        const int port = probe.serverPort();
        probe.close();
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("IVY_DATA_DIR", m_data.path());
        env.insert("IVY_HTTP_ADDR", QStringLiteral("127.0.0.1:%1").arg(port));
        env.insert("IVY_MASTER_KEYS", "k1:cUA297OzjWn7QmBrON/J1bDRS2v9NQJQUMTHFsS8Mwk=");
        env.insert("IVY_LOG_LEVEL", "error");
        m_server.setProcessEnvironment(env);
        m_server.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        m_server.start(bin, {"serve"});
        QVERIFY(m_server.waitForStarted());
        m_url = QUrl(QStringLiteral("http://127.0.0.1:%1").arg(port));
        ApiClient api(m_url);
        ApiReply r;
        for (int i = 0; i < 100; ++i) {
            r = call(api, "GET", "/health");
            if (r.ok) break;
            QTest::qWait(100);
        }
        QVERIFY2(r.ok, "server did not start");
        r = call(api, "GET", "/info");
        QVERIFY(!r.json.toObject().value("hasUsers").toBool());
        r = call(api, "POST", "/auth/register", {{"username", "tester"}, {"password", "correct horse battery"}});
        QVERIFY2(r.ok, qPrintable(r.error));
        m_token = r.json.toObject().value("token").toString();
        QVERIFY(!m_token.isEmpty());
    }

    void cleanupTestCase() {
        if (m_server.state() == QProcess::Running) {
            m_server.terminate();
            m_server.waitForFinished(5000);
        }
    }

    void firstSyncAndMerge() {
        Device a(m_url, m_token, "dev-a"), b(m_url, m_token, "dev-b");
        const Folder f = a.engine->createFolder("Work");
        const Note n = a.engine->createNote({.title = "Merge", .body = "one\ntwo\nthree\n", .folderId = f.id});
        QVERIFY(a.sync());
        QVERIFY(b.sync()); // snapshot
        QVERIFY(b.engine->notes().contains(n.id));
        QCOMPARE(b.engine->notes().value(n.id).folderId, f.id);
        QCOMPARE(b.engine->notes().value(n.id).rev, 1);

        a.engine->updateNote(n.id, {.body = QString("ONE\ntwo\nthree\n")});
        b.engine->updateNote(n.id, {.body = QString("one\ntwo\nTHREE\n")});
        QVERIFY(a.sync());
        QVERIFY(b.sync());
        QCOMPARE(b.engine->notes().value(n.id).body, QStringLiteral("ONE\ntwo\nTHREE\n"));
        QVERIFY(a.sync());
        QCOMPARE(a.engine->notes().value(n.id).body, QStringLiteral("ONE\ntwo\nTHREE\n"));
        QVERIFY(a.engine->outbox().isEmpty() && b.engine->outbox().isEmpty());
    }

    void sameLineMakesConflictCopy() {
        Device a(m_url, m_token, "dev-a"), b(m_url, m_token, "dev-b");
        QVERIFY(a.sync());
        const Note n = a.engine->createNote({.title = "Clash", .body = "line\n"});
        QVERIFY(a.sync());
        QVERIFY(b.sync());
        a.engine->updateNote(n.id, {.body = QString("from a\n")});
        b.engine->updateNote(n.id, {.body = QString("from b\n")});
        QVERIFY(a.sync());
        QSignalSpy conflict(b.engine.get(), &SyncEngine::conflict);
        QVERIFY(b.sync());
        QCOMPARE(conflict.size(), 1);
        QCOMPARE(b.engine->notes().value(n.id).body, QStringLiteral("from a\n"));
        const QString copyId = conflict.first().at(1).toString();
        QVERIFY(b.engine->notes().contains(copyId));
        QCOMPARE(b.engine->notes().value(copyId).body, QStringLiteral("from b\n"));
    }

    void metadataFieldsMerge() {
        Device a(m_url, m_token, "dev-a"), b(m_url, m_token, "dev-b");
        const Note n = a.engine->createNote({.title = "Meta"});
        QVERIFY(a.sync());
        QVERIFY(b.sync());
        a.engine->updateNote(n.id, {.title = QString("Meta renamed")});
        b.engine->updateNote(n.id, {.pinned = true});
        QVERIFY(a.sync());
        QVERIFY(b.sync());
        QVERIFY(a.sync());
        for (Device *d : {&a, &b}) {
            QCOMPARE(d->engine->notes().value(n.id).title, QStringLiteral("Meta renamed"));
            QVERIFY(d->engine->notes().value(n.id).pinned);
        }
    }

    void folderDeleteAndTombstones() {
        Device a(m_url, m_token, "dev-a"), b(m_url, m_token, "dev-b");
        const Folder top = a.engine->createFolder("Top");
        const Folder sub = a.engine->createFolder("Sub", top.id);
        const Note inSub = a.engine->createNote({.title = "inside", .folderId = sub.id});
        const Note doomed = a.engine->createNote({.title = "doomed"});
        QVERIFY(a.sync());
        QVERIFY(b.sync());
        b.engine->deleteFolder(top.id);
        a.engine->deleteNote(doomed.id);
        b.engine->updateNote(doomed.id, {.title = QString("edited on b")});
        QVERIFY(b.sync());
        QVERIFY(a.sync());
        QVERIFY(b.sync());
        for (Device *d : {&a, &b}) {
            QVERIFY(!d->engine->folders().contains(top.id));
            QVERIFY(!d->engine->folders().contains(sub.id));
            QVERIFY(d->engine->notes().value(inSub.id).trashed());
            QVERIFY(d->engine->notes().value(inSub.id).folderId.isEmpty());
            QVERIFY(!d->engine->notes().contains(doomed.id));
            QVERIFY(d->engine->outbox().isEmpty());
        }
    }

    void editsDuringPushAndBigBody() {
        Device a(m_url, m_token, "dev-a"), b(m_url, m_token, "dev-b");
        const Note n = a.engine->createNote({.title = "Big"});
        QVERIFY(a.sync());
        // Close to 5 MiB, full of quotes that grow when JSON-escaped.
        QString big;
        while (big.size() < (5 << 20) - 64) big += QStringLiteral("\"quoted\" \\ line\n");
        a.engine->updateNote(n.id, {.body = big});
        QVERIFY(a.sync());
        QVERIFY(a.engine->outbox().isEmpty());
        QCOMPARE(a.engine->state().blocked, 0);
        QVERIFY(b.sync());
        QCOMPARE(b.engine->notes().value(n.id).body.size(), big.size());

        a.engine->updateNote(n.id, {.body = QString("first\n")});
        QSignalSpy spy(a.engine.get(), &SyncEngine::roundFinished);
        a.engine->sync();
        a.engine->updateNote(n.id, {.body = QString("first\nsecond\n")}); // typed while in flight
        QVERIFY(spy.wait(20000));
        QVERIFY(a.sync());
        QVERIFY(a.engine->outbox().isEmpty());
        QVERIFY(b.sync());
        QCOMPARE(b.engine->notes().value(n.id).body, QStringLiteral("first\nsecond\n"));
    }

    void rejectedChangeStaysLocal() {
        Device a(m_url, m_token, "dev-a");
        const Note ok = a.engine->createNote({.title = "fine"});
        const Note bad = a.engine->createNote({.title = QString(400, u'x')}); // over 300 characters
        QSignalSpy rejected(a.engine.get(), &SyncEngine::rejected);
        QVERIFY(a.sync());
        QCOMPARE(rejected.size(), 1);
        QCOMPARE(a.engine->state().blocked, 1);
        QVERIFY(!a.engine->hasPending(EntityType::Note, ok.id));
        QVERIFY(a.engine->notes().contains(bad.id));
        a.engine->updateNote(bad.id, {.title = QString("short now")});
        QVERIFY(a.sync());
        QCOMPARE(a.engine->state().blocked, 0);
        QVERIFY(a.engine->outbox().isEmpty());
    }

    void uploadsDeduplicate() {
        Device a(m_url, m_token, "dev-a");
        AddedFile f1, f2;
        a.engine->addFile("same bytes", "a.txt", "text/plain", [&](AddedFile f) { f1 = f; });
        QTRY_VERIFY_WITH_TIMEOUT(f1.ok, 10000);
        a.engine->addFile("same bytes", "b.txt", "text/plain", [&](AddedFile f) { f2 = f; });
        QTRY_VERIFY_WITH_TIMEOUT(f2.ok, 10000);
        QVERIFY(!f1.local);
        QCOMPARE(f1.id, f2.id);
    }

    void liveEvents() {
        Device a(m_url, m_token, "dev-a"), b(m_url, m_token, "dev-b");
        QVERIFY(b.sync());
        EventStream events(b.api.get());
        QSignalSpy seq(&events, &EventStream::seq);
        events.start();
        QVERIFY(seq.wait(5000)); // the current seq on connect
        a.engine->createNote({.title = "live"});
        QVERIFY(a.sync());
        QTRY_VERIFY_WITH_TIMEOUT(seq.size() >= 2, 5000); // may already have come during the sync
        QVERIFY(seq.last().at(0).toLongLong() > b.engine->cursor());
        events.stop();
    }

    void badTokenIsUnauthorized() {
        Device d(m_url, "not-a-token", "dev-x");
        QSignalSpy unauth(d.engine.get(), &SyncEngine::unauthorized);
        d.engine->createNote({.title = "kept"});
        QVERIFY(!d.sync());
        QCOMPARE(unauth.size(), 1);
        QCOMPARE(d.engine->outbox().size(), 1); // kept for after signing in
    }
};

QTEST_GUILESS_MAIN(IntegrationTest)
#include "test_integration.moc"

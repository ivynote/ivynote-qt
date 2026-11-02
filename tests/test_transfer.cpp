// Moving notes between vaults: Markdown export/import and vault import.
#include "core/models.h"
#include "storage/secrets.h"
#include "sync/engine.h"
#include "vault/session.h"
#include "vault/transfer.h"
#include "vault/vaults.h"

#include <QDirIterator>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace {

Session *makeVault(const QString &name, bool encrypted) {
    VaultInfo v;
    v.name = name;
    v.encrypted = encrypted;
    QString err;
    if (!vaults::create(v, &err)) return nullptr;
    Session::OpenError e;
    return Session::open(v, {}, &e, &err);
}

bool run(TransferJob *job, QString *summary = nullptr) {
    QSignalSpy done(job, &TransferJob::finished);
    job->start();
    if (done.isEmpty() && !done.wait(10000)) return false;
    if (summary) *summary = done.first().at(1).toString();
    return done.first().at(0).toBool();
}

const Note *byTitle(SyncEngine *e, const QString &t) {
    for (const auto &n : e->notes())
        if (n.title == t) return &n;
    return nullptr;
}

} // namespace

class TransferTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;

private slots:
    void initTestCase() {
        vaults::setDataRoot(m_root.path());
        secrets::setFallbackOnly(true);
    }

    void exportImportRoundTrip() {
        std::unique_ptr<Session> src(makeVault("Source", true));
        QVERIFY(src);
        SyncEngine *e = src->engine();
        const Folder work = e->createFolder("Work");
        const Folder sub = e->createFolder("Sub", work.id);
        const Tag tag = e->createTag("idea");
        AddedFile file;
        e->addFile("\x89PNG fake", "pic one.png", "image/png", [&](AddedFile f) { file = f; });
        QTRY_VERIFY(file.ok);
        const Note target = e->createNote({.title = "Target", .body = "hello", .folderId = sub.id});
        e->createNote({.title = "Linker", .body = QStringLiteral("see [t](ivy://note/%1)\n\n![pic](ivy://res/%2)\n").arg(target.id, file.id),
                       .folderId = work.id, .tags = {tag.id}, .pinned = true});
        e->createNote({.title = "Trashed", .body = "x"});
        NotePatch trash;
        trash.trashed = true;
        e->updateNote(byTitle(e, "Trashed")->id, trash);

        QTemporaryDir out;
        QString summary;
        QVERIFY(run(transfer::exportMarkdown(src.get(), out.path()), &summary));
        QVERIFY2(QFile::exists(out.filePath("Work/Linker.md")), qPrintable(summary));
        QVERIFY(QFile::exists(out.filePath("Work/Sub/Target.md")));
        QVERIFY(QFile::exists(out.filePath("_attachments/pic one.png")));
        QVERIFY(!QFile::exists(out.filePath("Trashed.md")));
        QFile linker(out.filePath("Work/Linker.md"));
        QVERIFY(linker.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(linker.readAll());
        QVERIFY(text.contains("[t](Sub/Target.md)"));
        QVERIFY(text.contains("](../_attachments/pic%20one.png)"));
        QVERIFY(text.contains("tags: [\"idea\"]"));
        QVERIFY(text.contains("pinned: true"));

        std::unique_ptr<Session> dst(makeVault("Destination", false));
        QVERIFY(run(transfer::importMarkdown(dst.get(), {out.path()}, QString())));
        SyncEngine *d = dst->engine();
        const Note *l = byTitle(d, "Linker");
        const Note *t = byTitle(d, "Target");
        QVERIFY(l && t);
        QVERIFY(l->pinned);
        QCOMPARE(l->tags.size(), 1);
        QVERIFY(l->body.contains(QStringLiteral("ivy://note/") + t->id));
        QVERIFY(l->body.contains(QStringLiteral("![pic](ivy://res/")));
        QCOMPARE(d->attachments().size(), 1);
        // The exported folder becomes a notebook holding the tree.
        const auto path = d->folderPath(t->folderId);
        QCOMPARE(path.size(), 3);
        QCOMPARE(path[1]->name, QStringLiteral("Work"));
        QCOMPARE(path[2]->name, QStringLiteral("Sub"));
    }

    void importVaultCopiesEverything() {
        std::unique_ptr<Session> local(makeVault("Local notes", true));
        SyncEngine *e = local->engine();
        const Folder f = e->createFolder("Books");
        const Tag tag = e->createTag("read");
        AddedFile file;
        e->addFile("bytes", "a.pdf", "application/pdf", [&](AddedFile x) { file = x; });
        QTRY_VERIFY(file.ok);
        const Note a = e->createNote({.title = "A", .body = "x", .folderId = f.id, .tags = {tag.id}, .createdAt = 1700000000000});
        e->createNote({.title = "B", .body = QStringLiteral("[A](ivy://note/%1) [a.pdf](ivy://res/%2)").arg(a.id, file.id)});
        e->persistNow();
        const VaultInfo info = local->info();
        local.reset(); // closed, as when importing from another vault

        std::unique_ptr<Session> dst(makeVault("Synced", false));
        dst->engine()->createTag("READ"); // merged by name, case-insensitively
        QVERIFY(run(transfer::importVault(dst.get(), info)));
        SyncEngine *d = dst->engine();
        const Note *na = byTitle(d, "A");
        const Note *nb = byTitle(d, "B");
        QVERIFY(na && nb);
        QVERIFY(na->id != a.id);
        QCOMPARE(na->createdAt, 1700000000000);
        QCOMPARE(d->tags().size(), 1);
        QCOMPARE(na->tags.size(), 1);
        QVERIFY(nb->body.contains(QStringLiteral("ivy://note/") + na->id));
        QVERIFY(!nb->body.contains(file.id));
        QCOMPARE(d->attachments().size(), 1);
        const auto path = d->folderPath(na->folderId);
        QCOMPARE(path.size(), 2);
        QCOMPARE(path[0]->name, QStringLiteral("Local notes"));
        QCOMPARE(path[1]->name, QStringLiteral("Books"));
    }
};

QTEST_MAIN(TransferTest)
#include "test_transfer.moc"

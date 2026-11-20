// Moving notes between vaults: Markdown export/import and vault import.
#include "core/models.h"
#include "storage/secrets.h"
#include "sync/engine.h"
#include "vault/backup.h"
#include "vault/files.h"
#include "vault/session.h"
#include "vault/transfer.h"
#include "vault/vaults.h"

#include <QDirIterator>
#include <QFile>
#include <QSettings>
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

    void specializedMarkdownImports() {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkdir("_resources"));
        const auto write = [&](const QString &name, const QByteArray &data) {
            QFile file(dir.filePath(name));
            return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
        };
        QVERIFY(write("A.md", "---\ntitle: Different\n---\n[[B|other]] ![[pic.png]] #tag\n`[[B]]`"));
        QVERIFY(write("B.md", "note"));
        QVERIFY(write("_resources/pic.png", "image"));
        std::unique_ptr<Session> dst(makeVault("Obsidian", false));
        QVERIFY(run(transfer::importMarkdown(dst.get(), {dir.path()}, {}, dst.get(), transfer::MarkdownSource::Obsidian)));
        const auto *a = byTitle(dst->engine(), "A");
        const auto *b = byTitle(dst->engine(), "B");
        QVERIFY(a && b);
        QVERIFY(a->body.contains(QStringLiteral("[other](ivy://note/%1)").arg(b->id)));
        QVERIFY(a->body.contains(QStringLiteral("![pic.png](ivy://res/")));
        QVERIFY(a->body.contains(QStringLiteral("`[[B]]`")));
        QCOMPARE(a->tags.size(), 1);
        QVERIFY(write("_resources/example.md", "not a note"));
        QVERIFY(write("A.md", "![](./_resources/pic.png)"));
        std::unique_ptr<Session> joplin(makeVault("Joplin", false));
        QVERIFY(run(transfer::importMarkdown(joplin.get(), {dir.path()}, {}, joplin.get(), transfer::MarkdownSource::Joplin)));
        QCOMPARE(joplin->engine()->notes().size(), 2);
        QCOMPARE(joplin->engine()->attachments().size(), 1);
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

    void backupVolumePickerAndPathGuards() {
        QVERIFY(backup::isVirtualFileSystem(QStringLiteral("tmpfs"), QStringLiteral("/run")));
        QVERIFY(backup::isVirtualFileSystem(QStringLiteral("squashfs"), QStringLiteral("/snap/core/1")));
        QVERIFY(backup::isVirtualFileSystem(QStringLiteral("overlay"), QStringLiteral("/var/lib/docker")));
        QVERIFY(!backup::isVirtualFileSystem(QStringLiteral("ext4"), QStringLiteral("/data")));
        QVERIFY(!backup::isVirtualFileSystem(QStringLiteral("ntfs"), QStringLiteral("D:/")));

        const qint64 GiB = 1024LL * 1024 * 1024;
        QList<backup::VolumeCandidate> vols{
            {.rootPath = QStringLiteral("/snap/x"), .fileSystemType = QStringLiteral("squashfs"), .candidateBaseDir = QStringLiteral("/snap/x/IvynoteBackups"),
             .bytesTotal = 5000 * GiB, .bytesAvailable = 5000 * GiB, .valid = true, .ready = true, .readOnly = false, .writable = true},
            {.rootPath = QStringLiteral("/readonly"), .fileSystemType = QStringLiteral("ext4"), .candidateBaseDir = QStringLiteral("/readonly/IvynoteBackups"),
             .bytesTotal = 4000 * GiB, .bytesAvailable = 4000 * GiB, .valid = true, .ready = true, .readOnly = true, .writable = false},
            {.rootPath = QStringLiteral("/"), .fileSystemType = QStringLiteral("ext4"), .candidateBaseDir = QStringLiteral("/home/u/IvynoteBackups"),
             .bytesTotal = 256 * GiB, .bytesAvailable = 80 * GiB, .valid = true, .ready = true, .readOnly = false, .writable = true},
            {.rootPath = QStringLiteral("/mnt/big"), .fileSystemType = QStringLiteral("ext4"), .candidateBaseDir = QStringLiteral("/mnt/big/IvynoteBackups"),
             .bytesTotal = 2000 * GiB, .bytesAvailable = 1200 * GiB, .valid = true, .ready = true, .readOnly = false, .writable = true},
            {.rootPath = QStringLiteral("/mnt/huge_full"), .fileSystemType = QStringLiteral("ext4"), .candidateBaseDir = QStringLiteral("/mnt/huge_full/IvynoteBackups"),
             .bytesTotal = 8000 * GiB, .bytesAvailable = 50 * 1024 * 1024, .valid = true, .ready = true, .readOnly = false, .writable = true},
        };
        // Picks largest total writable disk that still has sufficient available space (skips huge_full with only 50 MiB free).
        const backup::VolumeCandidate best = backup::pickBestVolume(vols, backup::kLowSpaceWarningBytes);
        QVERIFY(best.valid);
        QCOMPARE(best.rootPath, QStringLiteral("/mnt/big"));

        // If all writable drives are below threshold, falls back to largest available space among writable drives.
        QList<backup::VolumeCandidate> tight{
            {.rootPath = QStringLiteral("/a"), .fileSystemType = QStringLiteral("ext4"), .candidateBaseDir = QStringLiteral("/a/IvynoteBackups"),
             .bytesTotal = 1000 * GiB, .bytesAvailable = 40 * 1024 * 1024, .valid = true, .ready = true, .readOnly = false, .writable = true},
            {.rootPath = QStringLiteral("/b"), .fileSystemType = QStringLiteral("ext4"), .candidateBaseDir = QStringLiteral("/b/IvynoteBackups"),
             .bytesTotal = 500 * GiB, .bytesAvailable = 200 * 1024 * 1024, .valid = true, .ready = true, .readOnly = false, .writable = true},
        };
        const backup::VolumeCandidate fallback = backup::pickBestVolume(tight, backup::kLowSpaceWarningBytes);
        QVERIFY(fallback.valid);
        QCOMPARE(fallback.rootPath, QStringLiteral("/b"));

        // Vault path guard prevents backing up inside vault dir or app vaults/ directory, while allowing sibling dirs sharing a prefix.
        VaultInfo v;
        v.id = QStringLiteral("v1234567890abcdef");
        QVERIFY(backup::isPathInsideVault(v.dir(), v));
        QVERIFY(backup::isPathInsideVault(v.dir() + QStringLiteral("/sub/backup"), v));
        QVERIFY(backup::isPathInsideVault(v.dir() + QStringLiteral("_backup"), v)); // still inside <dataRoot>/vaults/
        QVERIFY(!backup::isPathInsideVault(vaults::dataRoot() + QStringLiteral("/vaults_backup"), v));
    }

    void backupIncrementalExportAndRestore() {
        std::unique_ptr<Session> src(makeVault("Diary", true));
        QVERIFY(src);
        SyncEngine *e = src->engine();
        const Folder daily = e->createFolder("Daily");
        const Folder archive = e->createFolder("Archive");
        const Tag tag = e->createTag("journal");
        AddedFile file;
        e->addFile("\x89PNG fake-img-bytes", "photo.png", "image/png", [&](AddedFile f) { file = f; });
        QTRY_VERIFY(file.ok);

        const Note n1 = e->createNote({.title = "Day 1", .body = QStringLiteral("hello\n\n![p](ivy://res/%1)\n").arg(file.id),
                                       .folderId = daily.id, .tags = {tag.id}, .pinned = true, .createdAt = 1700000001000});
        const Note n2 = e->createNote({.title = "Day 2", .body = QStringLiteral("link [d1](ivy://note/%1)").arg(n1.id),
                                       .folderId = daily.id, .createdAt = 1700000002000});
        const Note n3 = e->createNote({.title = "Draft", .body = "to be trashed", .folderId = archive.id, .createdAt = 1700000003000});

        QTemporaryDir backupDir;
        backup::RunRecord rec1;
        QVERIFY(run(backup::exportMarkdownIncremental(src.get(), backupDir.path(), true, &rec1)));
        QVERIFY(rec1.ok);
        QCOMPARE(rec1.totalNotes, 3);
        QCOMPARE(rec1.writtenNotes, 3);
        QCOMPARE(rec1.unchangedNotes, 0);
        QCOMPARE(rec1.removedNotes, 0);
        QCOMPARE(rec1.totalAttachments, 1);
        QCOMPARE(rec1.writtenAttachments, 1);
        QCOMPARE(rec1.unchangedAttachments, 0);
        QVERIFY(QFile::exists(backupDir.filePath("Daily/Day 1.md")));
        QVERIFY(QFile::exists(backupDir.filePath("Daily/Day 2.md")));
        QVERIFY(QFile::exists(backupDir.filePath("Archive/Draft.md")));
        QVERIFY(QFile::exists(backupDir.filePath("_attachments/photo.png")));
        QVERIFY(QFile::exists(backupDir.filePath(QString::fromLatin1(backup::kManifestFileName))));

        // Create an untracked user file in the backup directory; incremental backup must never touch it.
        {
            QFile userFile(backupDir.filePath("my-personal-readme.txt"));
            QVERIFY(userFile.open(QIODevice::WriteOnly));
            userFile.write("keep me");
        }

        // Second run with no changes skips every note and attachment.
        backup::RunRecord rec2;
        QVERIFY(run(backup::exportMarkdownIncremental(src.get(), backupDir.path(), false, &rec2)));
        QVERIFY(rec2.ok);
        QCOMPARE(rec2.totalNotes, 3);
        QCOMPARE(rec2.writtenNotes, 0);
        QCOMPARE(rec2.unchangedNotes, 3);
        QCOMPARE(rec2.removedNotes, 0);
        QCOMPARE(rec2.writtenAttachments, 0);
        QCOMPARE(rec2.unchangedAttachments, 1);
        QVERIFY(QFile::exists(backupDir.filePath("my-personal-readme.txt")));

        // Edit Day 2, trash Draft (leaving Archive empty), and add Day 3.
        {
            NotePatch p;
            p.body = QStringLiteral("updated body [d1](ivy://note/%1)").arg(n1.id);
            e->updateNote(n2.id, p);
        }
        {
            NotePatch trash;
            trash.trashed = true;
            e->updateNote(n3.id, trash);
        }
        e->createNote({.title = "Day 3", .body = "brand new", .createdAt = 1700000004000});

        backup::RunRecord rec3;
        QVERIFY(run(backup::exportMarkdownIncremental(src.get(), backupDir.path(), true, &rec3)));
        QVERIFY(rec3.ok);
        QCOMPARE(rec3.totalNotes, 3);
        QCOMPARE(rec3.writtenNotes, 2);     // Day 2 updated + Day 3 added
        QCOMPARE(rec3.unchangedNotes, 1);   // Day 1 unchanged
        QCOMPARE(rec3.removedNotes, 1);     // Archive/Draft.md removed
        QCOMPARE(rec3.writtenAttachments, 0);
        QCOMPARE(rec3.unchangedAttachments, 1);
        QVERIFY(!QFile::exists(backupDir.filePath("Archive/Draft.md")));
        QVERIFY(!QDir(backupDir.filePath("Archive")).exists()); // empty old folder pruned
        QVERIFY(QFile::exists(backupDir.filePath("Day 3.md")));
        QVERIFY(QFile::exists(backupDir.filePath("my-personal-readme.txt")));

        // Manifest summary records history newest-first.
        const backup::ManifestSummary summary = backup::readManifestSummary(backupDir.path());
        QVERIFY(summary.exists);
        QVERIFY(summary.lastBackupAt > 0);
        QCOMPARE(summary.totalNotes, 3);
        QCOMPARE(summary.totalAttachments, 1);
        QCOMPARE(summary.history.size(), 3);

        // Synchronous flush works cleanly too.
        const backup::RunRecord syncRec = backup::runIncrementalSync(src.get(), backupDir.path(), false);
        QVERIFY(syncRec.ok);
        QCOMPARE(syncRec.writtenNotes, 0);
        QCOMPARE(syncRec.unchangedNotes, 3);

        // Backup folder can be directly restored via transfer::importMarkdown into a fresh vault.
        std::unique_ptr<Session> restored(makeVault("Restored", false));
        QVERIFY(run(transfer::importMarkdown(restored.get(), {backupDir.path()}, QString())));
        SyncEngine *r = restored->engine();
        const Note *r1 = byTitle(r, "Day 1");
        const Note *r2 = byTitle(r, "Day 2");
        const Note *r3 = byTitle(r, "Day 3");
        QVERIFY(r1 && r2 && r3);
        QVERIFY(r1->pinned);
        QCOMPARE(r1->tags.size(), 1);
        QVERIFY(r2->body.contains(QStringLiteral("updated body")));
        QVERIFY(r2->body.contains(QStringLiteral("ivy://note/") + r1->id));
        QCOMPARE(r->attachments().size(), 1);
    }

    void backupRemoteAndLocalVaultManagerLifecycle() {
        // Remote vault defaults to disabled (no startup prompt) and can be enabled & backed up manually.
        VaultInfo remoteInfo;
        remoteInfo.name = QStringLiteral("CloudVault");
        remoteInfo.kind = VaultInfo::Remote;
        remoteInfo.server = QStringLiteral("https://example.invalid");
        remoteInfo.username = QStringLiteral("alice");
        QString err;
        QVERIFY(vaults::create(remoteInfo, &err));
        Session::OpenError oe = Session::OpenError::None;
        std::unique_ptr<Session> remote(Session::open(remoteInfo, {}, &oe, &err));
        QVERIFY(remote != nullptr);
        remote->engine()->createNote({.title = QStringLiteral("Cloud Note"), .body = QStringLiteral("synced content")});

        LocalBackupManager mgr;
        QSignalSpy setupSpy(&mgr, &LocalBackupManager::needsInitialSetup);
        mgr.setSession(remote.get());
        QVERIFY(mgr.isActive());
        QVERIFY(!mgr.config().enabled);
        QVERIFY(mgr.config().configured);
        QVERIFY(!mgr.config().directory.isEmpty());
        QTest::qWait(50);
        QCOMPARE(setupSpy.count(), 0);

        QTemporaryDir dir;
        backup::Config remoteCfg = mgr.config();
        remoteCfg.enabled = true;
        remoteCfg.directory = dir.filePath(QStringLiteral("remote_bak"));
        mgr.updateConfig(remoteCfg);
        QVERIFY(backup::loadConfig(remote->info()).enabled);
        QCOMPARE(backup::loadConfig(remote->info()).directory, remoteCfg.directory);

        backup::RunRecord remoteRec;
        QVERIFY(run(backup::exportMarkdownIncremental(remote.get(), remoteCfg.directory, true, &remoteRec)));
        QVERIFY(remoteRec.ok);
        QCOMPARE(remoteRec.writtenNotes, 1);
        QVERIFY(QFile::exists(QDir(remoteCfg.directory).filePath(QStringLiteral("Cloud Note.md"))));
        QVERIFY(backup::runIncrementalSync(remote.get(), remoteCfg.directory, false).ok);

        // Local vault activates manager, defaults to enabled=true with auto-selected directory, and runs cleanly.
        std::unique_ptr<Session> local(makeVault("MyLocalVault", false));
        QVERIFY(local != nullptr);
        local->engine()->createNote({.title = "Hello", .body = "world"});
        const QString localId = local->info().id;

        mgr.setSession(local.get());
        QVERIFY(mgr.isActive());
        QVERIFY(mgr.config().enabled);
        QVERIFY(!mgr.config().configured);
        QVERIFY(!mgr.config().directory.isEmpty());

        backup::Config cfg = mgr.config();
        cfg.directory = dir.filePath("local_bak");
        cfg.configured = true;
        mgr.updateConfig(cfg);
        QCOMPARE(backup::loadConfig(local->info()).directory, cfg.directory);
        QVERIFY(backup::loadConfig(local->info()).configured);

        QSignalSpy finishedSpy(&mgr, &LocalBackupManager::backupFinished);
        mgr.runNow(true);
        QTRY_COMPARE(finishedSpy.count(), 1);
        QVERIFY(QFile::exists(QDir(cfg.directory).filePath("Hello.md")));

        // Removing the vault removes its QSettings backup config, while preserving exported Markdown files on disk.
        mgr.setSession(nullptr);
        local->close();
        local.reset();
        vaults::remove(localId);
        QVERIFY(!QSettings().contains(QStringLiteral("backup/%1/directory").arg(localId)));
        QVERIFY(QFile::exists(QDir(cfg.directory).filePath("Hello.md")));

        remote->close();
        remote.reset();
        vaults::remove(remoteInfo.id);
        QVERIFY(!QSettings().contains(QStringLiteral("backup/%1/directory").arg(remoteInfo.id)));
    }

    void defaultDataRootAndLegacyMigration() {
        QVERIFY(vaults::defaultDataRoot().endsWith(QStringLiteral("/app.ivynote.desktop")));

        QTemporaryDir sandbox;
        const QString legacy = sandbox.filePath(QStringLiteral("Ivynote/Ivynote"));
        const QString target = sandbox.filePath(QStringLiteral("app.ivynote.desktop"));
        QVERIFY(QDir().mkpath(legacy + QStringLiteral("/vaults/v1/files")));
        QVERIFY(QDir().mkpath(target + QStringLiteral("/libraries/lib1/files")));

        const auto write = [](const QString &path, const QByteArray &data) {
            QFile f(path);
            return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
        };
        QVERIFY(write(legacy + QStringLiteral("/vaults.json"), R"({"vaults":[{"id":"v1","name":"Old"}]})"));
        QVERIFY(write(legacy + QStringLiteral("/vaults/v1/vault.db"), "sqlite-bytes"));
        QVERIFY(write(legacy + QStringLiteral("/vaults/v1/files/f1"), "attachment-bytes"));
        QVERIFY(write(legacy + QStringLiteral("/secrets.json"), R"({"k":"v"})"));
        // Existing Tauri files in app.ivynote.desktop must stay completely untouched.
        QVERIFY(write(target + QStringLiteral("/libraries/lib1/files/a"), "tauri-file"));

        QVERIFY(vaults::migrateLegacyData(legacy, target));
        QVERIFY(QFile::exists(target + QStringLiteral("/vaults.json")));
        QVERIFY(QFile::exists(target + QStringLiteral("/vaults/v1/vault.db")));
        QVERIFY(QFile::exists(target + QStringLiteral("/vaults/v1/files/f1")));
        QVERIFY(QFile::exists(target + QStringLiteral("/secrets.json")));
        QVERIFY(QFile::exists(target + QStringLiteral("/libraries/lib1/files/a")));
        QVERIFY(!QFile::exists(legacy + QStringLiteral("/vaults.json")));

        // Idempotent on second call, and never overwrites an existing target registry.
        QVERIFY(!vaults::migrateLegacyData(legacy, target));
        QVERIFY(QDir().mkpath(legacy));
        QVERIFY(write(legacy + QStringLiteral("/vaults.json"), R"({"vaults":[{"id":"v2","name":"ShouldNotOverwrite"}]})"));
        QVERIFY(!vaults::migrateLegacyData(legacy, target));
        QFile reg(target + QStringLiteral("/vaults.json"));
        QVERIFY(reg.open(QIODevice::ReadOnly));
        QVERIFY(reg.readAll().contains("\"v1\""));
    }

    void customVaultLocationDiscoverScanRelocateAndSafeRemove() {
        QTemporaryDir external;
        const QString customDir = external.filePath(QStringLiteral("MyCustomVault"));
        QString err;

        // 1. Create a local vault directly at customDir (no extra subdirectory).
        VaultInfo custom;
        custom.name = QStringLiteral("CustomNotes");
        custom.kind = VaultInfo::Local;
        custom.path = customDir;
        QVERIFY2(vaults::create(custom, &err), qPrintable(err));
        QVERIFY(custom.hasCustomPath());
        QCOMPARE(custom.dir(), QDir::cleanPath(customDir));
        QVERIFY(QFile::exists(customDir + QStringLiteral("/vault.json")));

        {
            Session::OpenError oe = Session::OpenError::None;
            std::unique_ptr<Session> s(Session::open(custom, {}, &oe, &err));
            QVERIFY2(s != nullptr, qPrintable(err));
            const Note n = s->engine()->createNote({.title = QStringLiteral("Custom Note"), .body = QStringLiteral("Stored directly in custom folder")});
            QVERIFY(s->files()->putFile(QStringLiteral("att1234567890123"), QByteArray("hello-bytes")));
            QVERIFY(!n.id.isEmpty());
            s->engine()->persistNow();
            s->close();
        }
        QVERIFY(QFile::exists(customDir + QStringLiteral("/vault.db")));
        QVERIFY(QDir(customDir + QStringLiteral("/files")).exists());
        QVERIFY(backup::isPathInsideVault(customDir + QStringLiteral("/backups"), custom));

        // 2. Creating another vault inside or overlapping customDir must fail.
        {
            VaultInfo overlap;
            overlap.name = QStringLiteral("Overlap");
            overlap.kind = VaultInfo::Local;
            overlap.path = customDir + QStringLiteral("/nested");
            QVERIFY(!vaults::create(overlap, &err));
        }

        // 3. inspectDir verifies valid local vaults, legacy vaults (without vault.json),
        //    encrypted vaults, and rejects non-vault folders.
        {
            const DiscoveredVault d = vaults::inspectDir(customDir);
            QVERIFY(d.valid);
            QVERIFY(!d.encrypted);
            QVERIFY(!d.isRemote);
            QCOMPARE(d.name, QStringLiteral("CustomNotes"));
            QCOMPARE(d.noteRecords, 1);
            QCOMPARE(d.registeredId, custom.id);

            // Legacy vault without vault.json still passes inspection.
            QVERIFY(QFile::remove(customDir + QStringLiteral("/vault.json")));
            const DiscoveredVault legacy = vaults::inspectDir(customDir);
            QVERIFY(legacy.valid);
            QCOMPARE(legacy.noteRecords, 1);

            // Reopening recreates vault.json.
            Session::OpenError oe = Session::OpenError::None;
            std::unique_ptr<Session> s(Session::open(custom, {}, &oe, &err));
            QVERIFY(s != nullptr);
            s->close();
            QVERIFY(QFile::exists(customDir + QStringLiteral("/vault.json")));

            // Invalid dirs are rejected.
            const QString notAVault = external.filePath(QStringLiteral("NotAVault"));
            QVERIFY(QDir().mkpath(notAVault));
            QVERIFY(!vaults::inspectDir(notAVault).valid);
            QFile fakeDb(notAVault + QStringLiteral("/vault.db"));
            QVERIFY(fakeDb.open(QIODevice::WriteOnly));
            fakeDb.write("not a sqlite database at all, just junk bytes padded over 100 bytes................................................");
            fakeDb.close();
            QVERIFY(!vaults::inspectDir(notAVault).valid);
        }

        // Encrypted vault detection via meta.keycheck.
        {
            const QString encDir = external.filePath(QStringLiteral("EncryptedVault"));
            VaultInfo enc;
            enc.name = QStringLiteral("SecretVault");
            enc.kind = VaultInfo::Local;
            enc.encrypted = true;
            enc.path = encDir;
            QVERIFY2(vaults::create(enc, &err), qPrintable(err));
            Session::OpenError oe = Session::OpenError::None;
            std::unique_ptr<Session> s(Session::open(enc, {}, &oe, &err));
            QVERIFY2(s != nullptr, qPrintable(err));
            s->engine()->createNote({.title = QStringLiteral("Top Secret"), .body = QStringLiteral("Encrypted body")});
            s->engine()->persistNow();
            s->close();
            vaults::remove(enc.id, false);
            const DiscoveredVault d = vaults::inspectDir(encDir);
            QVERIFY(d.valid);
            QVERIFY(d.encrypted);
            QVERIFY(!d.isRemote);
            QVERIFY(d.registeredId.isEmpty());
        }

        // 4. Safe unlink (deleteFiles = false) keeps all files and allows re-attaching cleanly.
        const QString userFile = customDir + QStringLiteral("/unrelated-user-file.txt");
        {
            QFile f(userFile);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("keep me safe");
        }
        vaults::remove(custom.id, false);
        QVERIFY(vaults::find(custom.id).id.isEmpty());
        QVERIFY(QFile::exists(customDir + QStringLiteral("/vault.db")));
        QVERIFY(QFile::exists(userFile));

        VaultInfo reattached;
        QVERIFY2(vaults::attachExisting(customDir, QString(), &reattached, &err), qPrintable(err));
        QCOMPARE(reattached.name, QStringLiteral("CustomNotes"));
        QCOMPARE(reattached.dir(), QDir::cleanPath(customDir));
        // Re-attaching an already-registered vault deduplicates cleanly.
        VaultInfo reattachedAgain;
        QVERIFY(vaults::attachExisting(customDir, QString(), &reattachedAgain, &err));
        QCOMPARE(reattachedAgain.id, reattached.id);

        // 5. scanForVaults discovers custom vaults inside provided roots and skips hidden dirs.
        {
            const QString scanRoot = external.filePath(QStringLiteral("ScanArea"));
            const QString nestedVault = scanRoot + QStringLiteral("/Projects/2026/ScannedDiary");
            VaultInfo sv;
            sv.name = QStringLiteral("ScannedDiary");
            sv.kind = VaultInfo::Local;
            sv.path = nestedVault;
            QVERIFY(vaults::create(sv, &err));
            Session::OpenError oe = Session::OpenError::None;
            std::unique_ptr<Session> s(Session::open(sv, {}, &oe, &err));
            QVERIFY(s != nullptr);
            s->engine()->createNote({.title = QStringLiteral("Scanned Note"), .body = QStringLiteral("Found me!")});
            s->engine()->persistNow();
            s->close();
            vaults::remove(sv.id, false);

            const QList<DiscoveredVault> found = vaults::scanForVaults({scanRoot});
            QCOMPARE(found.size(), 1);
            QCOMPARE(found.first().dir, QDir::cleanPath(nestedVault));
            QCOMPARE(found.first().name, QStringLiteral("ScannedDiary"));
            QCOMPARE(found.first().noteRecords, 1);
        }

        // 6. Reopened re-attached custom vault preserves notes and attachments.
        {
            Session::OpenError oe = Session::OpenError::None;
            std::unique_ptr<Session> s(Session::open(vaults::find(reattached.id), {}, &oe, &err));
            QVERIFY2(s != nullptr, qPrintable(err));
            QCOMPARE(s->engine()->notes().size(), 1);
            QCOMPARE(s->engine()->notes().constBegin()->title, QStringLiteral("Custom Note"));
            QByteArray attData;
            QVERIFY(s->files()->readFile(QStringLiteral("att1234567890123"), &attData));
            QCOMPARE(attData, QByteArray("hello-bytes"));
            s->close();
        }

        // 7. Destructive remove (deleteFiles = true) on a custom folder removes only Ivynote artifacts.
        vaults::remove(reattached.id, true);
        QVERIFY(!QFile::exists(customDir + QStringLiteral("/vault.db")));
        QVERIFY(!QFile::exists(customDir + QStringLiteral("/vault.json")));
        QVERIFY(!QDir(customDir + QStringLiteral("/files")).exists());
        QVERIFY(QFile::exists(userFile));

        // 8. discoverDefaultVaults recovers unregistered local vaults in default dataRoot()/vaults
        //    and ignores remote vault caches.
        {
            const QString prevRoot = vaults::dataRoot();
            QTemporaryDir freshRoot;
            vaults::setDataRoot(freshRoot.path());

            VaultInfo defVault;
            defVault.name = QStringLiteral("DefaultVault");
            defVault.kind = VaultInfo::Local;
            QVERIFY(vaults::create(defVault, &err));
            {
                Session::OpenError oe = Session::OpenError::None;
                std::unique_ptr<Session> s(Session::open(defVault, {}, &oe, &err));
                QVERIFY(s != nullptr);
                s->engine()->createNote({.title = QStringLiteral("Survived"), .body = QStringLiteral("In default dir")});
                s->engine()->persistNow();
                s->close();
            }

            VaultInfo remoteCache;
            remoteCache.name = QStringLiteral("RemoteCache");
            remoteCache.kind = VaultInfo::Remote;
            remoteCache.server = QStringLiteral("https://example.invalid");
            remoteCache.username = QStringLiteral("dave");
            QVERIFY(vaults::create(remoteCache, &err));
            {
                Session::OpenError oe = Session::OpenError::None;
                std::unique_ptr<Session> s(Session::open(remoteCache, {}, &oe, &err));
                QVERIFY(s != nullptr);
                s->close();
            }

            // Simulate fresh registry while vault dirs still exist on disk.
            QVERIFY(QFile::remove(vaults::dataRoot() + QStringLiteral("/vaults.json")));
            QVERIFY(vaults::list().isEmpty());

            const QList<VaultInfo> discovered = vaults::discoverDefaultVaults();
            QCOMPARE(discovered.size(), 1);
            QCOMPARE(discovered.first().id, defVault.id);
            QCOMPARE(discovered.first().name, QStringLiteral("DefaultVault"));
            QVERIFY(!discovered.first().hasCustomPath());
            QCOMPARE(vaults::list().size(), 1);

            vaults::setDataRoot(prevRoot);
        }
    }
};

QTEST_MAIN(TransferTest)
#include "test_transfer.moc"

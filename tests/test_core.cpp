#include "core/hlc.h"
#include "core/agenda.h"
#include "core/ids.h"
#include "core/markdown.h"
#include "core/models.h"
#include "core/search.h"
#include "core/tree.h"
#include "storage/crypto.h"
#include "storage/localstore.h"
#include "sync/api.h"
#include "vault/files.h"

#include <QTemporaryDir>
#include <QtTest>

class CoreTest : public QObject {
    Q_OBJECT
private slots:
    void agendaSkipsCompletedDeletedAndCode() {
        Note n;
        n.id = QStringLiteral("note");
        n.body = QStringLiteral("- [x] done 📅 2026-01-01\n~~~md\n- [ ] sample 📅 2026-01-01\n~~~\n"
                                "- [ ] timed 📅 2026-11-20 8:15\n- [ ] all-day 📅 2026-11-19\n- [ ] invalid 📅 2026-02-31");
        Note trash = n;
        trash.trashedAt = 10;
        const auto tasks = agendaTasks({{n.id, n}, {QStringLiteral("trash"), trash}});
        QCOMPARE(tasks.size(), 2);
        QVERIFY(tasks.first().allDay);
        QCOMPARE(tasks.last().due.time(), QTime(8, 15));
        QCOMPARE(tasks.last().line, 4);
        QCOMPARE(tasks.last().text, QStringLiteral("timed"));
    }

    void editableTablesRoundTrip() {
        const QString source = QStringLiteral("| Name | Value |\n| :--- | ---: |\n| a\\|b | 10 |\n| `code` | 2 |");
        const md::Table table = md::parseTable(source);
        QVERIFY(table.valid());
        QCOMPARE(table.rows.size(), 3);
        QCOMPARE(table.rows[1][0], QStringLiteral("a|b"));
        QCOMPARE(table.align[1], int(Qt::AlignRight));
        const auto again = md::parseTable(md::formatTable(table));
        QCOMPARE(again.rows, table.rows);
        QCOMPARE(again.align, table.align);
        QVERIFY(!md::parseTable(QStringLiteral("not a table")).valid());
        const auto path = md::parseTable(QStringLiteral(R"MD(| A |
| --- |
| C:\\|)MD"));
        QVERIFY(path.valid());
        QCOMPARE(path.rows[1].size(), 1);
        QCOMPARE(md::parseTable(md::formatTable(path)).rows, path.rows);
    }

    void hlcFormatAndOrder() {
        qint64 t = 1793503505537;
        Hlc h(QStringLiteral("desk-1"), [&] { return t; });
        const QString a = h.tick();
        QCOMPARE(a, QStringLiteral("1793503505537-0000-desk-1"));
        const QString b = h.tick();
        QCOMPARE(b, QStringLiteral("1793503505537-0001-desk-1"));
        QVERIFY(b > a);
        h.observe(QStringLiteral("1793503509999-0003-other-node"));
        QCOMPARE(h.tick(), QStringLiteral("1793503509999-0004-desk-1"));
        qint64 wall;
        int counter;
        QString node;
        QVERIFY(Hlc::parse(QStringLiteral("1793503505537-00ff-a-b-c"), &wall, &counter, &node));
        QCOMPARE(counter, 255);
        QCOMPARE(node, QStringLiteral("a-b-c"));
    }

    void ids() {
        const QString id = ids::newId();
        QCOMPARE(id.size(), 16);
        QVERIFY(ids::valid(id));
        QVERIFY(!ids::valid(QStringLiteral("short")));
        QVERIFY(!ids::valid(QStringLiteral("../../etc/passwd")));
        QVERIFY(ids::valid(QStringLiteral("local-abcdefgh12345678")));
    }

    void wikiLinks() {
        const QString body = QStringLiteral("See [[Alpha]] and [[ beta  Note |alias]] and [[Gamma#Sec]]\n```\n[[InCode]]\n```\n`[[inline]]` ivy://note/abcdefgh12345678");
        QCOMPARE(md::wikiTargets(body), (QStringList{"alpha", "beta note", "gamma"}));
        QCOMPARE(md::noteRefs(body), QStringList{"abcdefgh12345678"});
        const auto w = md::parseWiki(QStringLiteral("Title#Head|Alias"));
        QCOMPARE(w.target, QStringLiteral("Title"));
        QCOMPARE(w.heading, QStringLiteral("Head"));
        QCOMPARE(w.alias, QStringLiteral("Alias"));
        QCOMPARE(md::resourceRefs(QStringLiteral("![a](ivy://res/abcdefgh1234) ```ivy://res/zzzzzzzz9999```")),
                 (QStringList{"abcdefgh1234", "zzzzzzzz9999"}));
    }

    void excerptAndWords() {
        QCOMPARE(md::excerpt(QStringLiteral("# Head\n\n**bold** [link](http://x) ![img](a.png) [[Wiki|Alias]]")),
                 QStringLiteral("Head bold link Alias"));
        QCOMPARE(md::wordCount(QStringLiteral("你好世界 hello world")), 6);
        QCOMPARE(md::wordCount(QStringLiteral("```\ncode here\n```\none")), 1);
    }

    void tasksSkipCode() {
        const QString body = QStringLiteral("- [ ] a\n```\n- [ ] code\n```\n- [x] b\n1. [ ] c");
        QCOMPARE(md::toggleTask(body, 1), QStringLiteral("- [ ] a\n```\n- [ ] code\n```\n- [ ] b\n1. [ ] c"));
        QCOMPARE(md::toggleTask(body, 2), QStringLiteral("- [ ] a\n```\n- [ ] code\n```\n- [x] b\n1. [x] c"));
        QCOMPARE(md::toggleTask(body, 9), body);
    }

    void tables() {
        QVERIFY(md::isTableDelimiter(QStringLiteral("|---|:-:|")));
        QVERIFY(md::isTableDelimiter(QStringLiteral("--- | ---:")));
        QVERIFY(!md::isTableDelimiter(QStringLiteral("---")));
        QVERIFY(!md::isTableDelimiter(QStringLiteral("| a | b |")));
        QCOMPARE(md::tableCells(QStringLiteral("| a | `x|y` | c \\| d |")), (QStringList{"a", "`x|y`", "c | d"}));
        QCOMPARE(md::tableAlignments(QStringLiteral("|:--|:-:|--:|---|")),
                 (QList<int>{Qt::AlignLeft, Qt::AlignHCenter, Qt::AlignRight, Qt::AlignLeft}));
        QCOMPARE(md::plainInline(QStringLiteral("**b** *i* `c` [[T|A]] [l](https://x)")), QStringLiteral("b i c A l"));
        // A table right under a paragraph line is split off; code is left alone.
        QCOMPARE(md::separateTables(QStringLiteral("text\n| a |\n|---|\n| 1 |")), QStringLiteral("text\n\n| a |\n|---|\n| 1 |"));
        const QString code = QStringLiteral("```\ntext\n| a |\n|---|\n```");
        QCOMPARE(md::separateTables(code), code);
        const QString plain = QStringLiteral("| a |\n|---|\n| 1 |\n| 2 |");
        QCOMPARE(md::separateTables(plain), plain);
    }

    void outlineAndSlug() {
        const auto o = md::outline(QStringLiteral("# One\ntext\n```\n# not\n```\n### Three ###"));
        QCOMPARE(o.size(), 2);
        QCOMPARE(o[1].level, 3);
        QCOMPARE(o[1].text, QStringLiteral("Three"));
        QCOMPARE(md::slug(QStringLiteral("Hello, World 你好")), QStringLiteral("hello-world-你好"));
    }

    void backlinks() {
        Note a, b, c;
        a.id = "aaaaaaaa"; a.title = "Alpha"; a.body = "links [[beta]]"; a.updatedAt = 1;
        b.id = "bbbbbbbb"; b.title = "Beta"; b.body = "self [[Beta]] and ivy://note/aaaaaaaa"; b.updatedAt = 2;
        c.id = "cccccccc"; c.title = "Gamma"; c.body = "[[Beta]]"; c.trashedAt = 5;
        const auto idx = md::buildLinkIndex({&a, &b, &c});
        QCOMPARE(idx.backlinks.value("bbbbbbbb"), QSet<QString>{"aaaaaaaa"});
        QCOMPARE(idx.backlinks.value("aaaaaaaa"), QSet<QString>{"bbbbbbbb"});
    }

    void search() {
        Note a, b;
        a.id = "aaaaaaaa"; a.title = "会议记录"; a.body = "讨论 同步 协议"; a.updatedAt = 1000;
        b.id = "bbbbbbbb"; b.title = "Other"; b.body = "nothing"; b.updatedAt = 2000;
        auto hits = searchNotes({&a, &b}, QStringLiteral("同步 会议"), 3000);
        QCOMPARE(hits.size(), 1);
        QCOMPARE(hits[0].note->id, QStringLiteral("aaaaaaaa"));
        QVERIFY(!hits[0].marks.isEmpty());
        QVERIFY(searchNotes({&a, &b}, QStringLiteral("同步 missing"), 3000).isEmpty());
    }

    void tree() {
        Folder f1, f2;
        f1.id = "f1f1f1f1"; f1.name = "Work"; f1.sort = 1;
        f2.id = "f2f2f2f2"; f2.name = "Sub"; f2.parentId = "f1f1f1f1";
        Note n1, n2, n3;
        n1.id = "n1n1n1n1"; n1.folderId = "f2f2f2f2"; n1.title = "Deep";
        n2.id = "n2n2n2n2"; n2.folderId = "missing!"; n2.title = "Orphan"; n2.pinned = true;
        n3.id = "n3n3n3n3"; n3.trashedAt = 9;
        TreeInput in;
        in.folders = {&f1, &f2};
        in.notes = {&n1, &n2, &n3};
        in.expanded = {"f1f1f1f1", "v:trash"};
        const auto rows = buildTree(in);
        QStringList keys;
        for (const auto &r : rows) keys << r.key;
        QCOMPARE(keys, (QStringList{"v:pinned", "h:notes", "f1f1f1f1", "f2f2f2f2", "/n2n2n2n2", "v:trash", "v:trash/n3n3n3n3"}));
        QCOMPARE(rows[2].count, 1); // counts include sub-notebooks
    }

    void cipherRoundTrip() {
        const QByteArray key = VaultCipher::generateKey();
        VaultCipher c(key);
        const QByteArray sealed = c.seal("hello", "ad1");
        QByteArray out;
        QVERIFY(c.open(sealed, "ad1", &out));
        QCOMPARE(out, QByteArray("hello"));
        QVERIFY(!c.open(sealed, "ad2", &out)); // bound to its place
        const QString rk = VaultCipher::encodeRecoveryKey(key);
        QCOMPARE(VaultCipher::decodeRecoveryKey(rk.toLower()), key);
        QVERIFY(VaultCipher::decodeRecoveryKey(QStringLiteral("AAAA")).isEmpty());

        QTemporaryDir dir;
        const QByteArray big(3 * (1 << 20) + 17, 'x');
        QVERIFY(c.writeFile(dir.filePath("f"), big, "files/f"));
        QVERIFY(c.readFile(dir.filePath("f"), "files/f", &out));
        QCOMPARE(out, big);
        QVERIFY(!c.readFile(dir.filePath("f"), "files/g", &out));
        // Truncation is detected.
        QFile f(dir.filePath("f"));
        QVERIFY(f.open(QIODevice::ReadWrite));
        f.resize(f.size() - 100);
        f.close();
        QVERIFY(!c.readFile(dir.filePath("f"), "files/f", &out));
    }

    void storeWrongKey() {
        QTemporaryDir dir;
        const QString path = dir.filePath("v.db");
        {
            LocalStore s(path, VaultCipher(VaultCipher::generateKey()));
            QCOMPARE(s.open(), LocalStore::OpenResult::Ok);
            StoreBatch b;
            b.puts.append({Table::Notes, "abcdefgh", QJsonObject{{"id", "abcdefgh"}, {"title", "secret title"}}});
            QVERIFY(s.apply(b));
            QCOMPARE(s.all(Table::Notes).size(), 1);
        }
        QFile raw(path);
        QVERIFY(raw.open(QIODevice::ReadOnly));
        QVERIFY(!raw.readAll().contains("secret title"));
        LocalStore other(path, VaultCipher(VaultCipher::generateKey()));
        QCOMPARE(other.open(), LocalStore::OpenResult::WrongKey);
        LocalStore plain(path, VaultCipher());
        QCOMPARE(plain.open(), LocalStore::OpenResult::WrongKey);
    }

    void serverAddress() {
        QCOMPARE(ApiClient::normalizeServer("notes.example.com/").toString(), QStringLiteral("https://notes.example.com"));
        QCOMPARE(ApiClient::normalizeServer("http://localhost:8080/api/v1/").toString(), QStringLiteral("http://localhost:8080"));
        QVERIFY(ApiClient::normalizeServer("ftp://x").isEmpty());
        QVERIFY(ApiClient::isLocalHost(QUrl("http://127.0.0.1:8080")));
        QVERIFY(!ApiClient::isLocalHost(QUrl("http://example.com")));
    }

    void changeWireForm() {
        OutboxEntry e;
        e.type = EntityType::Note;
        e.id = "abcdefgh";
        e.fields = QJsonObject{{"title", "t"}};
        e.hasBaseRev = true;
        e.baseRev = 3;
        QVERIFY(!e.toChange().contains("baseRev")); // only with a body
        e.fields.insert("body", "b");
        QCOMPARE(e.toChange().value("baseRev").toInt(), 3);
        e.isDelete = true;
        QVERIFY(!e.toChange().contains("fields"));
    }
};

QTEST_GUILESS_MAIN(CoreTest)
#include "test_core.moc"

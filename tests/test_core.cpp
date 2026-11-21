#include "core/agenda.h"
#include "core/diff.h"
#include "core/format.h"
#include "core/hlc.h"
#include "core/ids.h"
#include "core/insertitems.h"
#include "core/markdown.h"
#include "core/models.h"
#include "core/notelock.h"
#include "core/search.h"
#include "core/tasks.h"
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
        QCOMPARE(md::wordCount(QStringLiteral("Hello, world!")), 2);
        QCOMPARE(md::wordCount(QStringLiteral("It's a don’t-miss “event”.")), 4);
        QCOMPARE(md::wordCount(QStringLiteral("你好，世界！")), 6);
        QCOMPARE(md::wordCount(QStringLiteral("使用Word软件")), 5);
        QCOMPARE(md::wordCount(QStringLiteral("他说：“你好！”")), 8);
        QCOMPARE(md::wordCount(QStringLiteral("你好——世界……")), 8);
        QCOMPARE(md::wordCount(QStringLiteral("# 标题\n\n**加粗** 和 *斜体*\n\n---\n\n| 列一 | 列二 |\n| --- | --- |\n| 值一 | 值二 |")), 15);
        QCOMPARE(md::wordCount(QStringLiteral("```\ncode here\n```\none")), 1);
        QCOMPARE(md::wordCount(QStringLiteral(
            "东方\n"
            "人在世上，往往会碰上\n"
            "夜\t谈\t\n"
            "\"晦气\"或“幸运”。自然，谁都喜欢“幸运”而不要“晦气”。但“幸运”降落到谁头\n"
            "城隍抓“球”\t上会凭机遇，并非刻意追求\t\"可遇而不可求\".下面的故\t所能得,这就是人们常说的\t\n"
            "事就充分证明了这个道理。\n"
            "福建\t苏文恭\t\n"
            "有个孩子，出生时右额角上就长着一个红红的肉瘤子。随着他年龄的增长，肉瘤子也一天天长大。到他八岁时，肉瘤子已有小皮球那么大了。\n"
            "如果人人额上都长肉瘤子，那就没什么问题了。偏偏其他人没有，就他有，这就显得“出格”，“出格”就遭人嫌。于是，一家人都看不起他，父母亲也讨厌他。他在\n"
            "你就别余\t半点温暖。\t家里成了“臭猪头”，得不到\t\n"
            "一天晚饭后，无缘无故的母亲又指着儿子的肉瘤子大骂了起来，而且骂了个没完没了。这使孩子觉得非常伤心，就偷偷地离家出走\n"
            "成功\n"
            "去哪里呢?他不知道，只是沿着小道漫无目的地往\n"
            "W\n"
            "前走。走着走着，来到了城\n"
            "也是有感情的\t也很累，他就在庙门口坐下\t\n"
            "隍庙前。这时，天已漆黑，人\n"
            "图膜")), 396);
    }

    void noteStatistics() {
        const auto s = md::noteStats(QStringLiteral("# Title\n\nFirst paragraph with [[Link]] and [web](https://x.io).\n\n- [x] done\n- [ ] open\n\n```\ncode\n\nmore\n```\n![img](ivy://res/abcdefgh12)\n"));
        QCOMPARE(s.todos, 2);
        QCOMPARE(s.todosDone, 1);
        QCOMPARE(s.links, 2);
        QCOMPARE(s.media, 1);
        QCOMPARE(s.paragraphs, 6); // heading, text, two list items, the code block, the image line
        QVERIFY(s.readMinutes >= 1);
        QCOMPARE(md::noteStats(QString()).readMinutes, 0);
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

    void blobCryptoRoundTrip() {
        const QByteArray key = VaultCipher::generateKey();
        QByteArray sealed, plain;

        // Empty file still produces one final chunk (40 bytes).
        QVERIFY(BlobCrypto::encryptBlob(key, QStringLiteral("att00001"), QByteArray(), &sealed));
        QCOMPARE(sealed.size(), BlobCrypto::kOverhead);
        QCOMPARE(BlobCrypto::sealedSize(0), qint64(sealed.size()));
        QCOMPARE(BlobCrypto::plainSize(sealed.size()), qint64(0));
        QVERIFY(BlobCrypto::decryptBlob(key, QStringLiteral("att00001"), sealed, &plain));
        QVERIFY(plain.isEmpty());

        // Multi-chunk roundtrip with small chunkSize (64 bytes).
        QByteArray data(220, Qt::Uninitialized);
        for (int i = 0; i < data.size(); ++i) data[i] = char((i * 13 + 7) & 0xff);
        QVERIFY(BlobCrypto::encryptBlob(key, QStringLiteral("att00002"), data, &sealed, 64));
        QCOMPARE(qint64(sealed.size()), BlobCrypto::sealedSize(data.size(), 64));
        QCOMPARE(BlobCrypto::plainSize(sealed.size(), 64), qint64(data.size()));
        QVERIFY(BlobCrypto::decryptBlob(key, QStringLiteral("att00002"), sealed, &plain, 64));
        QCOMPARE(plain, data);

        // Wrong blobId or tampered tag fails authentication.
        QVERIFY(!BlobCrypto::decryptBlob(key, QStringLiteral("wrong-id"), sealed, &plain, 64));
        QByteArray tampered = sealed;
        tampered[tampered.size() - 1] = char(tampered.back() ^ 0x01);
        QVERIFY(!BlobCrypto::decryptBlob(key, QStringLiteral("att00002"), tampered, &plain, 64));
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

    void noteTitleTemplate() {
        const QDateTime dt(QDate(2026, 5, 6), QTime(9, 5, 3)); // 2026-05-06 is Wednesday
        QCOMPARE(fmt::formatNoteTitle(QString(), dt, true), QString());
        QCOMPARE(fmt::formatNoteTitle(QStringLiteral("YYYY-MM-DD HH:mm:ss"), dt, true), QStringLiteral("2026-05-06 09:05:03"));
        QCOMPARE(fmt::formatNoteTitle(QStringLiteral("YY/MM/DD-随记"), dt, true), QStringLiteral("26/05/06-随记"));
        QCOMPARE(fmt::formatNoteTitle(QStringLiteral("YYYY年MM月DD日 ddd"), dt, true), QStringLiteral("2026年05月06日 周三"));
        QCOMPARE(fmt::formatNoteTitle(QStringLiteral("YYYY-MM-DD ddd"), dt, false), QStringLiteral("2026-05-06 Wed"));
    }

    void diffAndConflictCounterparts() {
        QVERIFY(diff::isConflictNote(QStringLiteral("周报 (冲突副本 2026-05-01)")));
        QVERIFY(diff::isConflictNote(QStringLiteral("Report (conflict copy 2026-05-01)")));
        QVERIFY(!diff::isConflictNote(QStringLiteral("周报")));
        QCOMPARE(diff::conflictBaseTitle(QStringLiteral("周报 (冲突副本 2026-05-01)")), QStringLiteral("周报"));

        Note orig, copy, trashedCopy;
        orig.id = QStringLiteral("orig1111");
        orig.title = QStringLiteral("周报");
        orig.body = QStringLiteral("line1\nhello world\nline3");
        copy.id = QStringLiteral("copy2222");
        copy.title = QStringLiteral("周报 (冲突副本 2026-05-01)");
        copy.body = QStringLiteral("line1\nhello ivynote\nline3\nline4");
        trashedCopy = copy;
        trashedCopy.id = QStringLiteral("trash333");
        trashedCopy.trashedAt = 100;

        const QHash<QString, Note> all{{orig.id, orig}, {copy.id, copy}, {trashedCopy.id, trashedCopy}};
        const auto fromOrig = diff::findConflictCounterparts(orig, all);
        QCOMPARE(fromOrig.size(), 1);
        QCOMPARE(fromOrig.first()->id, copy.id);
        const auto fromCopy = diff::findConflictCounterparts(copy, all);
        QCOMPARE(fromCopy.size(), 1);
        QCOMPARE(fromCopy.first()->id, orig.id);

        const auto same = diff::compute(orig.body, orig.body);
        QVERIFY(same.identical);
        QCOMPARE(same.added, 0);
        QCOMPARE(same.removed, 0);

        const auto res = diff::compute(orig.body, copy.body);
        QVERIFY(!res.identical);
        QCOMPARE(res.removed, 1);
        QCOMPARE(res.added, 2);
        QCOMPARE(res.unchanged, 2);
        // Paired modification has intraline spans.
        bool sawPairedSpans = false;
        for (const auto &row : res.rows) {
            if (row.left && row.right && row.left->kind == diff::Kind::Del && row.right->kind == diff::Kind::Add) {
                if (!row.left->spans.isEmpty() && !row.right->spans.isEmpty()) sawPairedSpans = true;
            }
        }
        QVERIFY(sawPairedSpans);
    }

    void slashItemsMatchChineseEnglishAndPinyin() {
        QCOMPARE(insertitems::all().size(), 24);
        QCOMPARE(insertitems::match(QString()).size(), 24);
        QCOMPARE(insertitems::match(QStringLiteral("表格")).first().id, QStringLiteral("table"));
        QCOMPARE(insertitems::match(QStringLiteral("biaoge")).first().id, QStringLiteral("table"));
        QCOMPARE(insertitems::match(QStringLiteral("bg")).first().id, QStringLiteral("table"));
        QCOMPARE(insertitems::match(QStringLiteral("mermaid")).first().id, QStringLiteral("mermaid"));
        QCOMPARE(insertitems::match(QStringLiteral("h2")).first().id, QStringLiteral("h2"));
        QCOMPARE(insertitems::match(QStringLiteral("daiban")).first().id, QStringLiteral("todo"));
        QVERIFY(insertitems::match(QStringLiteral("zzzznonexistent")).isEmpty());
    }

    void advancedTasksRecurrenceAndNaturalDates() {
        const auto daily = tasks::parseRepeat(QStringLiteral("every day"));
        QVERIFY(daily.has_value());
        QCOMPARE(tasks::describeRepeat(*daily, true), QStringLiteral("每天"));
        QCOMPARE(tasks::describeRepeat(*daily, false), QStringLiteral("Every day"));

        const auto biweekly = tasks::parseRepeat(QStringLiteral("every 2 weeks"));
        QVERIFY(biweekly.has_value());
        QCOMPARE(tasks::nextDate(QDate(2026, 5, 1), *biweekly), QDate(2026, 5, 15));

        const auto monthly = tasks::parseRepeat(QStringLiteral("every month"));
        QVERIFY(monthly.has_value());
        QCOMPARE(tasks::nextDate(QDate(2026, 1, 31), *monthly), QDate(2026, 2, 28));

        const auto weekday = tasks::parseRepeat(QStringLiteral("every weekday"));
        QVERIFY(weekday.has_value());
        // 2026-05-01 is Friday -> next weekday is Monday 2026-05-04
        QCOMPARE(tasks::nextDate(QDate(2026, 5, 1), *weekday), QDate(2026, 5, 4));

        // Plain task toggle does not add ✅
        QCOMPARE(tasks::toggleTaskLine(QStringLiteral("- [ ] buy milk"), 0, QDate(2026, 5, 1)),
                 QStringLiteral("- [x] buy milk"));
        // Dated task toggle adds ✅ and uncheck removes ✅
        const QString doneDated = tasks::toggleTaskLine(QStringLiteral("- [ ] report 📅 2026-05-01"), 0, QDate(2026, 5, 1));
        QCOMPARE(doneDated, QStringLiteral("- [x] report 📅 2026-05-01 ✅ 2026-05-01"));
        QCOMPARE(tasks::toggleTaskLine(doneDated, 0, QDate(2026, 5, 1)),
                 QStringLiteral("- [ ] report 📅 2026-05-01"));
        // Recurring task spawns next occurrence immediately above
        const QString recur = tasks::toggleTaskLine(QStringLiteral("- [ ] standup 📅 2026-05-01 09:00 🔁 every weekday"), 0, QDate(2026, 5, 1));
        QCOMPARE(recur, QStringLiteral("- [ ] standup 📅 2026-05-04 09:00 🔁 every weekday\n"
                                       "- [x] standup 📅 2026-05-01 09:00 🔁 every weekday ✅ 2026-05-01"));

        // setTaskDue
        const QString withDue = tasks::setTaskDue(QStringLiteral("- [ ] pay rent"), tasks::Due{QDate(2026, 6, 1), {}}, QStringLiteral("every month"));
        QCOMPARE(withDue, QStringLiteral("- [ ] pay rent 📅 2026-06-01 🔁 every month"));
        QCOMPARE(tasks::setTaskDue(withDue, std::nullopt, QString()), QStringLiteral("- [ ] pay rent"));

        // Natural-language @ date parsing
        const QDateTime now(QDate(2026, 5, 6), QTime(10, 0)); // Wed May 6, 2026 10:00
        const auto tmrMorning = tasks::parseWhen(QStringLiteral("明天 9点"), now);
        QVERIFY(tmrMorning.has_value());
        QCOMPARE(tmrMorning->date, QDate(2026, 5, 7));
        QCOMPARE(tmrMorning->time, QTime(9, 0));

        const auto friPm = tasks::parseWhen(QStringLiteral("周五下午3点半"), now);
        QVERIFY(friPm.has_value());
        QCOMPARE(friPm->date, QDate(2026, 5, 8));
        QCOMPARE(friPm->time, QTime(15, 30));

        const auto in10m = tasks::parseWhen(QStringLiteral("10分钟后"), now);
        QVERIFY(in10m.has_value());
        QCOMPARE(in10m->date, QDate(2026, 5, 6));
        QCOMPARE(in10m->time, QTime(10, 10));

        const auto enTmr = tasks::parseWhen(QStringLiteral("tomorrow 14:30"), now);
        QVERIFY(enTmr.has_value());
        QCOMPARE(enTmr->date, QDate(2026, 5, 7));
        QCOMPARE(enTmr->time, QTime(14, 30));
    }

    void agendaTreeBucketsAndCounts() {
        const QDateTime now(QDate(2026, 5, 6), QTime(10, 0));
        Note n;
        n.id = QStringLiteral("n1");
        n.title = QStringLiteral("Plan");
        n.body = QStringLiteral(
            "- [ ] overdue task 📅 2026-05-05\n"
            "- [ ] today task 📅 2026-05-06 14:00\n"
            "- [x] done today 📅 2026-05-06 ✅ 2026-05-06\n"
            "- [ ] tomorrow task 📅 2026-05-07\n"
            "- [ ] this week 📅 2026-05-10\n"
            "- [ ] next month 📅 2026-06-15\n");

        Note enc;
        enc.id = QStringLiteral("n2");
        enc.title = QStringLiteral("Secret");
        enc.body = QStringLiteral("```ivy-encrypted\nv: 1\n```");

        const QHash<QString, Note> notes{{n.id, n}, {enc.id, enc}};
        const AgendaTree treeOpen = buildAgendaTree(notes, false, now, true);
        QCOMPARE(treeOpen.totalOpen, 5);
        QCOMPARE(treeOpen.overdueCount, 1);
        QCOMPARE(treeOpen.todayCount, 1);
        QCOMPARE(treeOpen.encryptedCount, 1);
        QCOMPARE(treeOpen.branches.size(), 5);
        QCOMPARE(treeOpen.branches[0].kind, AgendaBranch::Overdue);
        QVERIFY(treeOpen.branches[0].openByDefault());
        QCOMPARE(treeOpen.branches[1].kind, AgendaBranch::Today);
        QCOMPARE(treeOpen.branches[1].items.size(), 1);
        QCOMPARE(treeOpen.branches[2].kind, AgendaBranch::Tomorrow);
        QCOMPARE(treeOpen.branches[3].kind, AgendaBranch::Day);
        QVERIFY(!treeOpen.branches[3].openByDefault());
        QCOMPARE(treeOpen.branches[4].kind, AgendaBranch::Month);
        QVERIFY(!treeOpen.branches[4].openByDefault());

        const AgendaTree treeWithDone = buildAgendaTree(notes, true, now, true);
        QCOMPARE(treeWithDone.totalOpen, 5);
        QCOMPARE(treeWithDone.branches[1].items.size(), 2); // today open + done today
    }
};

QTEST_GUILESS_MAIN(CoreTest)
#include "test_core.moc"

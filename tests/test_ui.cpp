// Editor and renderer behavior (guide 13, "内容").
#include "ui/editor.h"
#include "ui/palette.h"
#include "ui/settingsdialog.h"
#include "ui/vaultdialog.h"
#include "storage/localstore.h"
#include "sync/engine.h"
#include "vault/files.h"
#include "vault/vaults.h"
#include <QTemporaryDir>
#include <QTabBar>
#include <QStackedWidget>
#include "ui/render.h"
#include "ui/theme.h"

#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QTextBlock>
#include <QtTest>

namespace {

const QString kSample = QStringLiteral(
    "# Title\n\n"
    "Some **bold**, *italic*, `code`, ~~gone~~ and ==marked== text.\n\n"
    "- [ ] open task\n- [x] done task\n- bullet\n1. one\n\n"
    "> quoted line\n\n"
    "![pic](ivy://res/abcdefgh12345678)\n\n"
    "```js\nconst a = '[[not a link]]';\n```\n\n"
    "See [[Other Note|alias]] and [site](https://example.com).\n\n---\n");

} // namespace

class UiTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { Theme::instance()->apply(); }

    void liveRenderingKeepsSourceExact() {
        MarkdownEditor e;
        e.resize(800, 600);
        e.show();
        for (auto mode : {MarkdownHighlighter::Live, MarkdownHighlighter::Source, MarkdownHighlighter::Live}) {
            e.setMode(mode);
            e.setMarkdown(kSample);
            QCOMPARE(e.markdown(), kSample);
            // Moving the cursor around reveals and hides syntax; text stays the same.
            e.setFocus();
            for (int i = 0; i < e.document()->blockCount(); ++i) {
                QTextCursor c(e.document()->findBlockByNumber(i));
                e.setTextCursor(c);
            }
            QCOMPARE(e.markdown(), kSample);
        }
        QVERIFY(!e.document()->isUndoAvailable()); // loading is not an edit
    }

    void remoteTextKeepsCursor() {
        MarkdownEditor e;
        e.setMarkdown(QStringLiteral("alpha\nbeta\ngamma\n"));
        QTextCursor c = e.textCursor();
        c.setPosition(QStringLiteral("alpha\nbeta\nga").size());
        e.setTextCursor(c);
        e.applyRemote(QStringLiteral("INSERTED\nalpha\nbeta\ngamma\n"));
        QCOMPARE(e.markdown(), QStringLiteral("INSERTED\nalpha\nbeta\ngamma\n"));
        QCOMPARE(e.textCursor().position(), int(QStringLiteral("INSERTED\nalpha\nbeta\nga").size()));
    }

    void enterContinuesLists() {
        MarkdownEditor e;
        e.show();
        e.setMarkdown(QStringLiteral("- [x] first"));
        QTextCursor c = e.textCursor();
        c.movePosition(QTextCursor::End);
        e.setTextCursor(c);
        QTest::keyClick(&e, Qt::Key_Return);
        QTest::keyClicks(&e, QStringLiteral("second"));
        QCOMPARE(e.markdown(), QStringLiteral("- [x] first\n- [ ] second"));
        QTest::keyClick(&e, Qt::Key_Return);
        QTest::keyClick(&e, Qt::Key_Return); // an empty item ends the list
        QCOMPARE(e.markdown(), QStringLiteral("- [x] first\n- [ ] second\n"));
        e.setMarkdown(QStringLiteral("9. nine"));
        c = e.textCursor();
        c.movePosition(QTextCursor::End);
        e.setTextCursor(c);
        QTest::keyClick(&e, Qt::Key_Return);
        QCOMPARE(e.markdown(), QStringLiteral("9. nine\n10. "));
    }

    void formattingShortcuts() {
        MarkdownEditor e;
        e.show();
        e.setMarkdown(QStringLiteral("word"));
        QTextCursor c = e.textCursor();
        c.select(QTextCursor::Document);
        e.setTextCursor(c);
        QTest::keyClick(&e, Qt::Key_B, Qt::ControlModifier);
        QCOMPARE(e.markdown(), QStringLiteral("**word**"));
        QTest::keyClick(&e, Qt::Key_B, Qt::ControlModifier); // toggles back
        QCOMPARE(e.markdown(), QStringLiteral("word"));
        e.setHeading(2);
        QCOMPARE(e.markdown(), QStringLiteral("## word"));
        e.setHeading(2);
        QCOMPARE(e.markdown(), QStringLiteral("word"));
        e.toggleList(QStringLiteral("- [ ]"));
        QCOMPARE(e.markdown(), QStringLiteral("- [ ] word"));
    }

    void pasteIsPlainText() {
        MarkdownEditor e;
        e.setMarkdown(QString());
        auto *mime = new QMimeData;
        mime->setHtml(QStringLiteral("<b>rich</b>"));
        mime->setText(QStringLiteral("rich\r\nline"));
        QApplication::clipboard()->setMimeData(mime);
        e.paste();
        QCOMPARE(e.markdown(), QStringLiteral("rich\nline"));
    }

    // Builds the dialogs (and saves pictures of them when IVY_SHOTS_DIR is set).
    void dialogsBuild() {
        QTemporaryDir dir;
        vaults::setDataRoot(dir.path());
        const QString shots = qEnvironmentVariable("IVY_SHOTS_DIR");
        auto save = [&](QWidget *w, const QString &name) {
            w->show();
            QTest::qWait(1600);
            if (!shots.isEmpty()) w->grab().save(shots + u'/' + name + QStringLiteral(".png"));
        };
        VaultDialog choose;
        save(&choose, "vault-choose");
        VaultDialog server(nullptr, true);
        save(&server, "vault-server");
        SettingsDialog settings(nullptr);
        save(&settings, "settings");

        LocalStore store(dir.filePath("v.db"), VaultCipher());
        QCOMPARE(store.open(), LocalStore::OpenResult::Ok);
        VaultFiles files(dir.path(), VaultCipher());
        SyncEngine engine(&store, &files, nullptr, "dev");
        engine.init();
        engine.createNote({.title = "会议记录", .body = "讨论同步协议与冲突副本"});
        engine.createNote({.title = "Sync notes", .body = "merge rules"});
        QWidget host;
        host.resize(900, 640);
        host.show();
        auto *palette = new CommandPalette(&engine, {{"新建笔记", "Ctrl+N", "edit", {}}}, &host);
        palette->open(QStringLiteral("同步"));
        QTest::qWait(300);
        if (!shots.isEmpty()) palette->grab().save(shots + QStringLiteral("/palette.png"));
        palette->close();
    }

    void rendererIsSafe() {
        const QString html = renderMarkdown(QStringLiteral(
            "<script>alert(1)</script>\n\n[x](javascript:alert(1)) <img src=x onerror=alert(1)>\n\n"
            "![remote](https://tracker.example/p.png) [ok](https://example.com) [[Wiki Target#Sec|Alias]]"));
        QVERIFY(!html.contains(QLatin1String("<script")));
        QVERIFY(!html.contains(QLatin1String("javascript:")));
        QVERIFY(!html.contains(QLatin1String("<img src=\"x\"")));
        QVERIFY(!html.contains(QLatin1String("<img src=\"https://tracker")));
        QVERIFY(html.contains(QLatin1String("href=\"https://example.com\"")));
        QVERIFY(html.contains(QLatin1String("href=\"ivy-wiki:Wiki%20Target%23Sec\"")));
        QVERIFY(html.contains(QLatin1String(">Alias</a>")));
    }

    void rendererTasksAndFiles() {
        RenderOptions o;
        o.interactiveTasks = true;
        o.resourceKind = [](const QString &) { return QStringLiteral("file"); };
        o.resourceName = [](const QString &) { return QStringLiteral("doc.pdf"); };
        const QString html = renderMarkdown(QStringLiteral("- [ ] a\n- [x] b\n\n![doc.pdf](ivy://res/abcdefgh12345678)"), o);
        QVERIFY(html.contains(QLatin1String("ivy-task:0")));
        QVERIFY(html.contains(QLatin1String("ivy-task:1")));
        QVERIFY(html.contains(QLatin1String("class=\"file\"")));
    }
};

QTEST_MAIN(UiTest)
#include "test_ui.moc"

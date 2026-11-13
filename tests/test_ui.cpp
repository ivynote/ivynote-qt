// Editor and renderer behavior (guide 13, "内容").
#include "ui/editor.h"
#include "ui/notepane.h"
#include "ui/remoteimages.h"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <cstring>
#include <QBuffer>
#include <QPainter>
#include <QSplitter>
#include <QLineEdit>
#include <QToolButton>
#include "ui/markdownview.h"
#include "core/markdown.h"
#include <QTextTable>
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

class ImageReply : public QNetworkReply {
public:
    ImageReply(const QNetworkRequest &request, QByteArray data, bool fail, QObject *parent)
        : QNetworkReply(parent), m_data(std::move(data)) {
        setRequest(request);
        setUrl(request.url());
        open(QIODevice::ReadOnly);
        QTimer::singleShot(0, this, [this, fail] {
            if (fail) setError(QNetworkReply::TemporaryNetworkFailureError, QStringLiteral("offline"));
            else emit readyRead();
            setFinished(true);
            emit finished();
        });
    }
    void abort() override {}
    qint64 bytesAvailable() const override { return m_data.size() - m_offset + QNetworkReply::bytesAvailable(); }
protected:
    qint64 readData(char *buffer, qint64 size) override {
        const qint64 n = std::min<qint64>(size, m_data.size() - m_offset);
        if (!n) return -1;
        std::memcpy(buffer, m_data.constData() + m_offset, size_t(n));
        m_offset += n;
        return n;
    }
private:
    QByteArray m_data;
    qint64 m_offset = 0;
};

class ImageNetwork : public QNetworkAccessManager {
public:
    QByteArray png;
    bool fail = true;
    int requests = 0;
protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override {
        ++requests;
        return new ImageReply(request, png, fail, this);
    }
};

const QString kSample = QStringLiteral(
    "# Title\n\n"
    "Some **bold**, *italic*, `code`, ~~gone~~ and ==marked== text.\n\n"
    "- [ ] open task\n- [x] done task\n- bullet\n1. one\n\n"
    "> quoted line\n\n"
    "![pic](ivy://res/abcdefgh12345678)\n\n"
    "```js\nconst a = '[[not a link]]';\n```\n\n"
    "See [[Other Note|alias]] and [site](https://example.com).\n\n---\n"
    "Before\n| a | **b** |\n|:--|--:|\n| 1 | [[x]] |\n");

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

    void modeAndSizeKeepUndo() {
        MarkdownEditor e;
        e.setMarkdown(QStringLiteral("# title\n\ntext"));
        e.moveCursor(QTextCursor::End);
        QTest::keyClicks(&e, QStringLiteral(" added"));
        e.setMode(MarkdownHighlighter::Source);
        e.setFontSize(20);
        e.setMode(MarkdownHighlighter::Live);
        QVERIFY(e.document()->isUndoAvailable());
        e.undo();
        QCOMPARE(e.markdown(), QStringLiteral("# title\n\ntext"));
        e.redo();
        QCOMPARE(e.markdown(), QStringLiteral("# title\n\ntext added"));
    }

    void fencesKeepLiteralContent() {
        const QString body = QStringLiteral("````md\n```\n~~~\n# not a heading\n- [ ] literal\n[[not a link]]\n````\n# Real\n- [ ] real");
        MarkdownEditor e;
        e.setMarkdown(body);
        QCOMPARE(md::outline(body).size(), 1);
        QVERIFY(md::wikiTargets(body).isEmpty());
        QCOMPARE(md::toggleTask(body, 0), QString(body).replace(QStringLiteral("[ ] real"), QStringLiteral("[x] real")));
        for (int i = 1; i <= 5; ++i) {
            const auto *info = static_cast<BlockInfo *>(e.document()->findBlockByNumber(i).userData());
            QVERIFY(info);
            QCOMPARE(info->kind, BlockInfo::Code);
        }
        e.setTextCursor(QTextCursor(e.document()->findBlockByNumber(4)));
        e.moveCursor(QTextCursor::EndOfBlock);
        QTest::keyClick(&e, Qt::Key_Return);
        QVERIFY(e.markdown().contains(QStringLiteral("- [ ] literal\n\n[[")));
    }

    void indentSelectionExcludesNextLine() {
        MarkdownEditor e;
        e.setMarkdown(QStringLiteral("one\ntwo\nthree"));
        QTextCursor c(e.document());
        c.setPosition(4, QTextCursor::KeepAnchor);
        e.setTextCursor(c);
        QTest::keyClick(&e, Qt::Key_Tab);
        QCOMPARE(e.markdown(), QStringLiteral("  one\ntwo\nthree"));
    }

    void highlightDoesNotRewriteAttributesOrCode() {
        const QString html = renderMarkdown(QStringLiteral("[link](https://example.com/==path==) ==mark== `==literal==`"));
        QVERIFY(html.contains(QStringLiteral("href=\"https://example.com/==path==\"")));
        QVERIFY(html.contains(QStringLiteral("<span class=\"mark\">mark</span>")));
        QVERIFY(html.contains(QStringLiteral("<code>==literal==</code>")));
    }

    void previewImagesFollowWidth() {
        MarkdownView view;
        view.image = [](const QString &) { QImage img(1600, 800, QImage::Format_RGB32); img.fill(Qt::green); return img; };
        view.resize(800, 600);
        view.show();
        view.setMarkdown(QStringLiteral("![test](ivy://res/abcdefgh12345678)"), {});
        QTest::qWait(120);
        auto width = [&] { return view.document()->resource(QTextDocument::ImageResource, QUrl(QStringLiteral("ivy://res/abcdefgh12345678"))).value<QImage>().width(); };
        const int wide = width();
        view.resize(320, 600);
        QTest::qWait(180);
        QVERIFY(width() < wide);
        QVERIFY(width() <= view.viewport()->width());
    }

    void imagesParseCommonMarkAndLegacyLinks() {
        const QString url = QStringLiteral("https://example.com/photo(1).png?x=1&y=2");
        for (const QString &markdown : {
                 QStringLiteral("![](<%1>)").arg(url),
                 QStringLiteral("![alt](%1 \"caption\")").arg(url),
                 QStringLiteral("![alt](<%1> 'caption')").arg(url)}) {
            QCOMPARE(md::standaloneImage(markdown).url, url);
            MarkdownEditor editor;
            editor.setMarkdown(markdown + QStringLiteral("\n\ntext"));
            const auto *info = static_cast<BlockInfo *>(editor.document()->begin().userData());
            QVERIFY(info);
            QCOMPARE(info->kind, BlockInfo::Image);
            QCOMPARE(info->imageId, url);
            QCOMPARE(editor.markdown(), markdown + QStringLiteral("\n\ntext"));
        }
        const QString old = QStringLiteral("# [%1](%1)").arg(url);
        QVERIFY(md::standaloneImage(old).legacy);
        RenderOptions opts;
        opts.remoteImages = true;
        const QString html = renderMarkdown(old, opts);
        QVERIFY(html.contains(QStringLiteral("<img src=")));
        QVERIFY(!html.contains(QStringLiteral("<h1>")));
        const QString ordinary = QStringLiteral("[Read the article](https://example.com/photo.png)");
        QVERIFY(md::standaloneImage(ordinary).url.isEmpty());
        QVERIFY(renderMarkdown(ordinary, opts).contains(QStringLiteral("<a href=")));
        QVERIFY(!renderMarkdown(QStringLiteral("```md\n") + old + QStringLiteral("\n```"), opts).contains(QStringLiteral("<img ")));
    }

    void imageParagraphAndReferenceKeepSource() {
        MarkdownEditor editor;
        editor.resize(700, 600);
        editor.image = [](const QString &) { QImage image(400, 200, QImage::Format_RGB32); image.fill(Qt::green); return image; };
        const QString body = QStringLiteral("Text ![alt](ivy://res/abcdefgh12345678) after\n\n![ref][pic]\n\n[pic]: https://example.com/photo.png");
        editor.setMarkdown(body);
        const auto *paragraph = static_cast<BlockInfo *>(editor.document()->begin().userData());
        QVERIFY(paragraph && paragraph->kind == BlockInfo::ImageParagraph);
        QVERIFY(paragraph->imageDocument);
        QVERIFY(paragraph->imageSize.height() >= 200);
        const auto *reference = static_cast<BlockInfo *>(editor.document()->findBlockByNumber(2).userData());
        QVERIFY(reference && reference->kind == BlockInfo::Image);
        QCOMPARE(reference->imageId, QStringLiteral("https://example.com/photo.png"));
        QCOMPARE(editor.markdown(), body);
    }

    void editingImageReferenceUpdatesDisplay() {
        MarkdownEditor editor;
        editor.setMarkdown(QStringLiteral("![ref][pic]\n\n[pic]: https://example.com/first.png"));
        QTest::qWait(150);
        QTextCursor c(editor.document()->lastBlock());
        c.select(QTextCursor::LineUnderCursor);
        c.insertText(QStringLiteral("[pic]: https://example.com/second.png"));
        QTRY_COMPARE(static_cast<BlockInfo *>(editor.document()->begin().userData())->imageId, QStringLiteral("https://example.com/second.png"));
    }

    void remoteImageLoadsAndRetries() {
        ImageNetwork network;
        QImage photo(90, 50, QImage::Format_RGB32);
        photo.fill(QColor(20, 160, 70));
        QBuffer buffer(&network.png);
        buffer.open(QIODevice::WriteOnly);
        QVERIFY(photo.save(&buffer, "PNG"));
        RemoteImages images(nullptr, &network);
        const QString url = QStringLiteral("https://example.test/photo(1).png");
        QVERIFY(images.image(url).isNull());
        QTRY_VERIFY(images.failed(url));
        QCOMPARE(network.requests, 1);
        QVERIFY(images.image(url).isNull());
        QCOMPARE(network.requests, 1); // repaint does not repeatedly retry
        network.fail = false;
        images.retryFailed();
        QTRY_VERIFY(!images.image(url).isNull());
        QCOMPARE(images.image(url).size(), photo.size());
        QCOMPARE(images.image(url).pixelColor(0, 0), photo.pixelColor(0, 0));
        QCOMPARE(network.requests, 2);
    }

    void insertMenuCommandsKeepMarkdown() {
        MarkdownEditor editor;
        editor.setMarkdown(QString());
        editor.insertElement(QStringLiteral("table"));
        QVERIFY(renderMarkdown(editor.markdown()).contains(QStringLiteral("<table")));
        editor.undo();
        QCOMPARE(editor.markdown(), QString());
        editor.setMarkdown(QStringLiteral("a ``` delimiter"));
        editor.selectAll();
        editor.insertElement(QStringLiteral("codeblock"));
        QVERIFY(editor.markdown().startsWith(QStringLiteral("````\na ``` delimiter\n````")));
        editor.setReadOnly(true);
        const QString before = editor.markdown();
        editor.insertElement(QStringLiteral("table"));
        QCOMPARE(editor.markdown(), before);
    }

    void tableCellEditingAndNavigation() {
        MarkdownEditor editor;
        editor.resize(800, 600);
        const QString original = QStringLiteral("Before\n\n| A | B |\n|---|---|\n| one | two |\n\nAfter");
        editor.setMarkdown(original);
        editor.show();
        editor.setTextCursor(QTextCursor(editor.document()->findBlockByNumber(2)));
        QVERIFY(editor.editTableCell(1, 0));
        auto *cell = editor.findChild<QLineEdit *>(QStringLiteral("tableCellEditor"));
        QVERIFY(cell && cell->isVisible());
        QTest::keyClicks(cell, QStringLiteral("a | b"));
        QTest::keyClick(cell, Qt::Key_Tab);
        QVERIFY(editor.markdown().contains(QStringLiteral("a \\| b")));
        QVERIFY(cell->isVisible());
        QCOMPARE(cell->text(), QStringLiteral("two"));
        QTest::keyClick(cell, Qt::Key_Escape);
        QVERIFY(editor.markdown().startsWith(QStringLiteral("Before\n\n")));
        QVERIFY(editor.markdown().endsWith(QStringLiteral("\n\nAfter")));
        editor.undo();
        QCOMPARE(editor.markdown(), original);
        editor.redo();
        QVERIFY(editor.markdown().contains(QStringLiteral("a \\| b")));
        editor.setTextCursor(QTextCursor(editor.document()->findBlockByNumber(2)));
        QVERIFY(editor.editTableCell(1, 1));
        QTest::keyClick(cell, Qt::Key_Tab);
        const auto table = md::parseTable(editor.markdown().section(QStringLiteral("\n\n"), 1, 1));
        QCOMPARE(table.rows.size(), 3);
        QTest::keyClicks(cell, QStringLiteral("new"));
        editor.commitPendingCell(); // save even if a close/flush happens before focus-out
        QVERIFY(editor.markdown().contains(QStringLiteral("new")));
    }

    void closingEditorWithFocusedTableCell() {
        auto *editor = new MarkdownEditor;
        editor->resize(800, 600);
        editor->setMarkdown(QStringLiteral("| A | B |\n|---|---|\n| one | two |"));
        editor->show();
        editor->activateWindow();
        QVERIFY(editor->editTableCell(1, 0));
        QPointer<QLineEdit> cell = editor->findChild<QLineEdit *>(QStringLiteral("tableCellEditor"));
        QVERIFY(cell);
        cell->setFocus();
        QTest::qWait(30);
        QTest::keyClicks(cell, QStringLiteral("saved before closing"));
        delete editor;
        QVERIFY(cell.isNull());
        QCoreApplication::processEvents();
    }

    void tableTypingWritesThroughAndFlushKeepsCellOpen() {
        NotePane pane;
        pane.resize(1000, 700);
        for (QWidget *child : pane.findChildren<QWidget *>(Qt::FindDirectChildrenOnly)) child->setEnabled(true);
        pane.editor()->setReadOnly(false);
        pane.editor()->setMarkdown(QStringLiteral("| A | B |\n|---|---|\n| old | other |"));
        pane.show();
        QVERIFY(pane.editor()->editTableCell(1, 0));
        auto *cell = pane.findChild<QLineEdit *>(QStringLiteral("tableCellEditor"));
        QVERIFY(cell);
        QTest::keyClicks(cell, QStringLiteral("new"));
        QCOMPARE(md::parseTable(pane.editor()->markdown()).rows[1][0], QStringLiteral("new"));
        pane.flush();
        QVERIFY(cell->isVisible());
        // An unrelated remote cell change is kept when editing continues.
        pane.editor()->applyRemote(pane.editor()->markdown().replace(QStringLiteral("other"), QStringLiteral("remote")));
        QTest::keyClicks(cell, QStringLiteral("er"));
        const auto table = md::parseTable(pane.editor()->markdown());
        QCOMPARE(table.rows[1][0], QStringLiteral("newer"));
        QCOMPARE(table.rows[1][1], QStringLiteral("remote"));
    }

    void tableStructureAndSort() {
        MarkdownEditor editor;
        editor.setMarkdown(QStringLiteral("| N |\n|---|\n| 10 |\n| text |\n| 2 |\n| NaN |"));
        QVERIFY(editor.tableAction(QStringLiteral("sort-ascending")));
        auto table = md::parseTable(editor.markdown());
        QCOMPARE(table.rows[1][0], QStringLiteral("2"));
        QCOMPARE(table.rows[2][0], QStringLiteral("10"));
        QVERIFY(editor.tableAction(QStringLiteral("column-after")));
        table = md::parseTable(editor.markdown());
        QCOMPARE(table.align.size(), 2);
        QVERIFY(editor.tableAction(QStringLiteral("center")));
        QCOMPARE(md::parseTable(editor.markdown()).align[0], int(Qt::AlignHCenter));
        editor.setReadOnly(true);
        QVERIFY(!editor.tableAction(QStringLiteral("row-after")));
    }

    void replaceAllIsOneUndoAndDoesNotRescanReplacement() {
        NotePane pane;
        pane.resize(900, 650);
        for (QWidget *child : pane.findChildren<QWidget *>(Qt::FindDirectChildrenOnly)) child->setEnabled(true);
        pane.editor()->setReadOnly(false);
        pane.editor()->setMarkdown(QStringLiteral("alpha Alpha alpha"));
        pane.show();
        pane.showReplace();
        auto *find = pane.findChild<QLineEdit *>(QStringLiteral("noteFindText"));
        auto *replacement = pane.findChild<QLineEdit *>(QStringLiteral("noteReplaceText"));
        auto *all = pane.findChild<QToolButton *>(QStringLiteral("noteReplaceAll"));
        QVERIFY(find && replacement && all);
        find->setText(QStringLiteral("alpha"));
        replacement->setText(QStringLiteral("alphaalpha"));
        QTest::mouseClick(all, Qt::LeftButton);
        QCOMPARE(pane.editor()->markdown(), QStringLiteral("alphaalpha alphaalpha alphaalpha"));
        pane.editor()->undo();
        QCOMPARE(pane.editor()->markdown(), QStringLiteral("alpha Alpha alpha"));
        pane.editor()->setReadOnly(true);
        replacement->setText(QStringLiteral("forbidden"));
        QTest::mouseClick(all, Qt::LeftButton);
        QCOMPARE(pane.editor()->markdown(), QStringLiteral("alpha Alpha alpha"));
    }

    void findInNoteDoesNotEdit() {
        NotePane pane;
        pane.resize(900, 650);
        for (QWidget *child : pane.findChildren<QWidget *>(Qt::FindDirectChildrenOnly)) child->setEnabled(true);
        pane.setEnabled(true);
        pane.editor()->setEnabled(true);
        pane.editor()->setMarkdown(QStringLiteral("alpha Beta alpha"));
        pane.show();
        pane.showFind();
        auto *input = pane.findChild<QLineEdit *>(QStringLiteral("noteFindText"));
        QVERIFY(input);
        input->setText(QStringLiteral("alpha"));
        QCOMPARE(pane.editor()->textCursor().selectedText(), QStringLiteral("alpha"));
        QCOMPARE(pane.editor()->extraSelections().size(), 2);
        const int first = pane.editor()->textCursor().selectionStart();
        QTest::keyClick(input, Qt::Key_Return);
        QVERIFY(pane.editor()->textCursor().selectionStart() != first);
        QTest::keyClick(input, Qt::Key_Return);
        QCOMPARE(pane.editor()->textCursor().selectionStart(), first);
        QCOMPARE(pane.editor()->markdown(), QStringLiteral("alpha Beta alpha"));
    }

    void variableBackticksStayLiteral() {
        MarkdownEditor e;
        e.setMarkdown(QStringLiteral("``[[literal]] and `tick` and **bold**``"));
        const auto *info = static_cast<BlockInfo *>(e.document()->begin().userData());
        QVERIFY(info);
        QVERIFY(info->links.isEmpty());
        QCOMPARE(e.markdown(), QStringLiteral("``[[literal]] and `tick` and **bold**``"));
    }

    void formattedHeadingAnchors() {
        const QString html = renderMarkdown(QStringLiteral("## **A** & B"));
        QVERIFY(html.contains(QStringLiteral("name=\"a-b\"")));
    }

    void tasksUseSameOrderAsPreview() {
        const QString body = QStringLiteral("中文\n\n    - [ ] code\n\n> - [ ] quoted\n\n- [ ] outer\n  - [x] nested");
        const QString changed = md::toggleTask(body, 0);
        QVERIFY(changed.contains(QStringLiteral("    - [ ] code")));
        QVERIFY(changed.contains(QStringLiteral("> - [x] quoted")));
        QVERIFY(md::toggleTask(body, 2).contains(QStringLiteral("  - [ ] nested")));
    }

    void tableVisualFixture() {
        const QString shots = qEnvironmentVariable("IVY_SHOTS_DIR");
        if (shots.isEmpty()) return;
        MarkdownEditor editor;
        editor.resize(850, 600);
        editor.setMarkdown(QStringLiteral("# 表格编辑\n\n点击单元格编辑，Tab 切换，右键管理行列。\n\n| 项目 | 状态 | 数量 |\n| --- | :---: | ---: |\n| Markdown 编辑器 | 进行中 | 10 |\n| 图片展示 | 已完成 | 2 |\n\n表格后面的文字保持原样。"));
        editor.show();
        editor.setTextCursor(QTextCursor(editor.document()->findBlockByNumber(4)));
        QVERIFY(editor.editTableCell(1, 1));
        QTest::qWait(150);
        editor.grab().save(shots + QStringLiteral("/editable-table.png"));
    }

    void imageVisualFixture() {
        const QString shots = qEnvironmentVariable("IVY_SHOTS_DIR");
        if (shots.isEmpty()) return;
        QImage photo(640, 280, QImage::Format_RGB32);
        photo.fill(QColor("#196c4d"));
        {
            QPainter painter(&photo);
            painter.setPen(Qt::white);
            painter.setFont(Theme::uiFont(32));
            painter.drawText(photo.rect(), Qt::AlignCenter, QStringLiteral("Ivynote · Markdown image"));
        }
        const QString body = QStringLiteral("# 图片展示\n\n![](<ivy://res/abcdefgh12345678> \"caption\")\n\n图文混排 ![示例](ivy://res/abcdefgh12345678) 图片之后的文字。\n\n![reference][pic]\n\n[pic]: ivy://res/abcdefgh12345678\n");
        QSplitter split;
        split.resize(1440, 980);
        auto *editor = new MarkdownEditor(&split);
        auto *preview = new MarkdownView(&split);
        editor->image = [photo](const QString &) { return photo; };
        preview->image = [photo](const QString &) { return photo; };
        editor->setMarkdown(body);
        preview->setMarkdown(body, {});
        split.show();
        preview->setFocus();
        QTest::qWait(200);
        split.grab().save(shots + QStringLiteral("/images-live-preview.png"));
        editor->setMode(MarkdownHighlighter::Source);
        QTest::qWait(100);
        split.grab().save(shots + QStringLiteral("/images-source-preview.png"));
        editor->setMode(MarkdownHighlighter::Live);
        const auto original = Theme::instance()->mode();
        Theme::instance()->setMode(Theme::Dark);
        QTest::qWait(150);
        split.grab().save(shots + QStringLiteral("/images-dark-preview.png"));
        Theme::instance()->setMode(original);
        QCOMPARE(editor->markdown(), body);
    }

    void editorVisualFixture() {
        const QString shots = qEnvironmentVariable("IVY_SHOTS_DIR");
        if (shots.isEmpty()) return;
        MarkdownEditor editor;
        editor.resize(760, 850);
        editor.setMarkdown(kSample);
        editor.show();
        editor.clearFocus();
        QTest::qWait(150);
        editor.grab().save(shots + QStringLiteral("/editor-live.png"));
        editor.setMode(MarkdownHighlighter::Source);
        editor.grab().save(shots + QStringLiteral("/editor-source.png"));
        MarkdownView preview;
        preview.resize(760, 850);
        preview.setMarkdown(kSample, {});
        preview.show();
        QTest::qWait(150);
        preview.grab().save(shots + QStringLiteral("/editor-preview.png"));
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
        // The view shows remote images; other schemes still never load.
        RenderOptions view;
        view.remoteImages = true;
        const QString shown = renderMarkdown(QStringLiteral("![a](https://img.example/p.png) ![b](file:///etc/passwd)"), view);
        QVERIFY(shown.contains(QLatin1String("<img src=\"https://img.example/p.png\" alt=\"a\">")));
        QVERIFY(!shown.contains(QLatin1String("file:")));
    }

    void rendererTables() {
        // GitHub lets a table follow a paragraph line directly.
        const QString html = renderMarkdown(QStringLiteral("Intro\n| a | b |\n|---|--:|\n| 1 | 2 |"));
        QVERIFY(html.contains(QLatin1String("<table")));
        QVERIFY(html.contains(QLatin1String("<p>Intro</p>")));
    }

    void viewTaskLists() {
        RenderOptions o;
        o.forView = true;
        o.interactiveTasks = true;
        const QString html = renderMarkdown(QStringLiteral("- [ ] a\n- [x] b\n  - [ ] c\n- plain\n\n---\n\n```\nx\n```"), o);
        // Rows instead of list items: no bullet next to the box.
        QVERIFY(!html.contains(QLatin1String("<li")));
        QVERIFY(html.contains(QLatin1String("ivy-check:0")));
        QVERIFY(html.contains(QLatin1String("ivy-check:1")));
        QVERIFY(html.contains(QLatin1String("ivy-task:2")));
        QVERIFY(html.contains(QLatin1String("class=\"td\""))); // done item muted and struck
        QVERIFY(html.contains(QLatin1String("class=\"hr\"")));
        QVERIFY(html.contains(QLatin1String("<pre><code>x</code></pre>")));
        // Plain HTML for the clipboard keeps ordinary lists.
        QVERIFY(renderMarkdown(QStringLiteral("- [x] b")).contains(QLatin1String("<li")));
        QVERIFY(renderMarkdown(QStringLiteral("- [x] b")).contains(QStringLiteral("☑")));
    }

    void hangingIndentFollowsEdits() {
        MarkdownEditor e;
        e.resize(800, 600);
        e.show();
        e.setMode(MarkdownHighlighter::Live);
        e.setMarkdown(QStringLiteral("text"));
        QCOMPARE(e.document()->begin().blockFormat().leftMargin(), 0.0);
        QTextCursor c(e.document());
        e.setTextCursor(c);
        e.insertPlainText(QStringLiteral("> "));
        // Wrapped lines of a quote hang under its text.
        const qreal hang = e.document()->begin().blockFormat().leftMargin();
        QVERIFY(hang > 0);
        QCOMPARE(e.document()->begin().blockFormat().textIndent(), -hang);
        e.undo();
        QCOMPARE(e.markdown(), QStringLiteral("text"));
        QCOMPARE(e.document()->begin().blockFormat().leftMargin(), 0.0);
        e.redo();
        QCOMPARE(e.document()->begin().blockFormat().leftMargin(), hang);
    }

    void liveTableKeepsGridAtCaret() {
        MarkdownEditor e;
        e.resize(800, 600);
        e.show();
        e.setMode(MarkdownHighlighter::Live);
        e.setMarkdown(QStringLiteral("x\n\n| a | b |\n|---|---|\n| 1 | 2 |\n| 3 | 4 |\n\ny"));
        for (int i = 2; i <= 5; ++i) {
            const auto *info = static_cast<BlockInfo *>(e.document()->findBlockByNumber(i).userData());
            QVERIFY(info && info->kind == BlockInfo::Table);
            QCOMPARE(info->tableRow, i - 2);
        }
        e.activateWindow();
        e.setFocus();
        QVERIFY(QTest::qWaitForWindowActive(&e));
        e.setTextCursor(QTextCursor(e.document()->findBlockByNumber(4)));
        // A caret in a live table keeps the grid; cell editing is an overlay.
        for (int i = 2; i <= 5; ++i) QVERIFY(!static_cast<BlockInfo *>(e.document()->findBlockByNumber(i).userData())->active);
        QVERIFY(!static_cast<BlockInfo *>(e.document()->findBlockByNumber(0).userData())->active);
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

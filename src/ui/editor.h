// The Markdown editor: plain text in, plain text out. Live and source modes
// share it; the split view pairs it (source) with a rendered preview.
#pragma once

#include "ui/highlighter.h"

#include <QHash>
#include <QImage>
#include <QTextEdit>
#include <functional>

class QCompleter;
class QPainter;
class QStringListModel;

class MarkdownEditor : public QTextEdit {
    Q_OBJECT
public:
    explicit MarkdownEditor(QWidget *parent = nullptr);

    void setMarkdown(const QString &text); // loads a note; resets undo
    QString markdown() const { return toPlainText(); }
    // Replaces the text with the smallest edit, keeping the cursor in place.
    void applyRemote(const QString &text);

    void setMode(MarkdownHighlighter::Mode m);
    MarkdownHighlighter::Mode mode() const { return m_hl->mode(); }
    void setFontSize(int px);
    void setColumnWidth(int maxWidth);
    // The text column inside the editor, in its own coordinates.
    QRect textColumn() const;
    void setWikiTitles(const QStringList &titles);
    void refreshAttachment(const QString &id);
    void insertBlockText(const QString &markdown); // on its own line at the cursor
    void scrollToLine(int line);
    void toggleFormat(const QString &mark);
    void setHeading(int level);
    void toggleList(const QString &kind); // "1." / "-" / "- [ ]"

    // Wiring to the vault.
    std::function<QImage(const QString &id)> image;
    std::function<QSize(const QString &id)> imageMeta;
    std::function<QString(const QString &id)> attachmentKind;
    std::function<bool(const QString &title)> wikiExists;

signals:
    void edited();
    void linkActivated(const QString &target, bool wiki);
    void filesDropped(const QStringList &paths);
    void imagePasted(const QImage &image);
    void cursorLineChanged(int line);
    void columnChanged();

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    bool canInsertFromMimeData(const QMimeData *source) const override;
    void insertFromMimeData(const QMimeData *source) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void focusInEvent(QFocusEvent *e) override;
    void focusOutEvent(QFocusEvent *e) override;

private:
    void updateActive();
    void applyLineHeight();
    void updateMargins();
    void updateCompletion();
    void insertCompletion(const QString &title);
    bool continueList();
    const LinkSpan *linkAt(const QPoint &pos, QTextBlock *blockOut) const;
    int taskAt(const QPoint &pos, QTextBlock *blockOut) const;
    QRectF charRect(const QTextBlock &block, int pos) const;
    struct TableGeometry {
        int cols = 0;
        QList<qreal> x;   // column edges from the table's left, cols + 1 of them
        QList<int> align; // per column
    };
    TableGeometry tableGeometry(const QTextBlock &start) const;
    void paintTableRow(QPainter &p, const QTextBlock &block, const QRectF &r, QHash<int, TableGeometry> &cache) const;

    MarkdownHighlighter *m_hl;
    QCompleter *m_completer;
    QStringListModel *m_titles;
    bool m_loading = false;
    int m_maxColumn = 680;
    int m_px = 16;
    int m_lastLine = -1;
};

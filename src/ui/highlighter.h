// Live rendering for the Markdown editor (guide 11.3). The document stays
// plain Markdown: only character formats change. On lines without the cursor
// syntax is shrunk to nothing and drawn over (checkboxes, bullets, rules,
// images); the cursor's line shows its source.
#pragma once

#include <QHash>
#include <QImage>
#include <QSharedPointer>
#include <QTimer>
#include <QSet>
#include <QSize>
#include <QSyntaxHighlighter>
#include <QTextBlockUserData>
#include <functional>

struct LinkSpan {
    int start = 0; // within the block
    int end = 0;
    QString target; // URL, "ivy://..." or wiki inner text
    bool wiki = false;
};

struct TaskMarkerSpan {
    enum Kind { Due, Repeat, Done };
    int start = 0; // within the block
    int end = 0;
    Kind kind = Due;
};

class BlockInfo : public QTextBlockUserData {
public:
    enum Kind { Text, Fence, Code, Heading, Quote, Rule, Image, ImageParagraph, ReferenceDefinition, Table };
    Kind kind = Text;
    int level = 0;
    int tableRow = -1; // 0 header, 1 delimiter, 2… body
    bool active = false;
    QList<QPair<int, bool>> tasks; // position of '[' and whether done
    QList<int> taskBoxes;          // where each task's checkbox is drawn
    QList<int> bullets;            // position of the list marker
    qreal hang = 0;                // hanging indent of wrapped lines (lists, quotes)
    QList<LinkSpan> links;
    QList<TaskMarkerSpan> taskMarkers;
    QString imageId;
    QSharedPointer<QTextDocument> imageDocument;
    QSize imageSize; // reserved display size
};

class MarkdownHighlighter : public QSyntaxHighlighter {
    Q_OBJECT
public:
    enum Mode { Live, Source };
    explicit MarkdownHighlighter(QTextDocument *doc);

    void setMode(Mode m);
    Mode mode() const { return m_mode; }
    void setFontSize(int px);
    int fontSize() const { return m_px; }
    // Which blocks show their source (the cursor's lines).
    void setActive(int first, int last);
    int activeFirst() const { return m_first; }
    int activeLast() const { return m_last; }
    void setColumnWidth(int w) { m_columnWidth = w; }
    void setViewportHeight(int h) { m_viewportHeight = h; }

    struct TableGeometry {
        int start = -1;            // block number of the header
        int cols = 0;
        QList<qreal> x;            // column edges from the table's left, cols + 1
        QList<int> align;          // per column
        QList<qreal> rowHeight;    // per row, delimiter included
    };
    TableGeometry tableGeometry(const QTextBlock &row) const;
    int columnWidth() const { return m_columnWidth; }
    // Blocks whose kind or hanging indent changed since the last call.
    QList<int> takeChanged() { QList<int> l(m_changed.begin(), m_changed.end()); m_changed.clear(); return l; }

    std::function<bool(const QString &title)> wikiExists;
    std::function<QImage(const QString &id)> imageLoader;
    std::function<QSize(const QString &id)> imageSize; // natural size or invalid
    std::function<QString(const QString &id)> attachmentKind;

protected:
    void highlightBlock(const QString &text) override;

private:
    void hide(int start, int len);
    qreal advance(const QTextCharFormat &f, const QString &text) const;
    QTextCharFormat tallChar(qreal height, qreal lineFactor = 1.75) const;
    bool tableBlock(const QString &text, BlockInfo *info, bool live, bool active, const QFont &mono);
    void inlineSpans(const QString &text, int from, BlockInfo *info, bool live, const QTextCharFormat &base);
    QSize displaySize(const QString &id) const;

    QTimer m_referenceTimer;
    QString m_referenceDefinitions;
    Mode m_mode = Live;
    int m_px = 16;
    int m_first = -1, m_last = -1;
    int m_columnWidth = 680;
    int m_viewportHeight = 800;
    mutable QHash<int, TableGeometry> m_tables;
    QSet<int> m_changed;
    mutable int m_tableRevision = -1, m_tablePx = 0, m_tableWidth = 0;
};

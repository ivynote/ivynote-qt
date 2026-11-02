// Live rendering for the Markdown editor (guide 11.3). The document stays
// plain Markdown: only character formats change. On lines without the cursor
// syntax is shrunk to nothing and drawn over (checkboxes, bullets, rules,
// images); the cursor's line shows its source.
#pragma once

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

class BlockInfo : public QTextBlockUserData {
public:
    enum Kind { Text, Fence, Code, Heading, Quote, Rule, Image };
    Kind kind = Text;
    int level = 0;
    bool active = false;
    QList<QPair<int, bool>> tasks; // position of '[' and whether done
    QList<int> bullets;            // position of the list marker
    QList<LinkSpan> links;
    QString imageId;
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
    void setColumnWidth(int w) { m_columnWidth = w; }
    void setViewportHeight(int h) { m_viewportHeight = h; }

    std::function<bool(const QString &title)> wikiExists;
    std::function<QSize(const QString &id)> imageSize; // natural size or invalid
    std::function<QString(const QString &id)> attachmentKind;

protected:
    void highlightBlock(const QString &text) override;

private:
    void hide(int start, int len);
    void inlineSpans(const QString &text, int from, BlockInfo *info, bool live, const QTextCharFormat &base);
    QSize displaySize(const QString &id) const;

    Mode m_mode = Live;
    int m_px = 16;
    int m_first = -1, m_last = -1;
    int m_columnWidth = 680;
    int m_viewportHeight = 800;
};

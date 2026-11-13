// Markdown text helpers shared with the web client: links, excerpts, word
// counts, outline and task toggling. All work offline on the raw text.
#pragma once

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

struct Note;

namespace md {

QString normalizeTitle(const QString &t);
QString stripCode(const QString &body);

struct WikiLink {
    QString target;
    QString alias;
    QString heading;
};
WikiLink parseWiki(const QString &inner);

QStringList wikiTargets(const QString &body);   // normalized, distinct, code skipped
QStringList noteRefs(const QString &body);      // ivy://note/<id>, code skipped
QStringList resourceRefs(const QString &body);  // ivy://res/<id>, code included

QString excerpt(const QString &md, int max = 120);
int wordCount(const QString &md);
QString slug(const QString &text);

struct Heading {
    int level = 1;
    QString text;
    int line = 0; // 0-based block number
};
QList<Heading> outline(const QString &body);

// Toggles the index-th task checkbox (code blocks skipped). Returns body
// unchanged when there is no such task.
QString toggleTask(const QString &body, int index);

QString linkContext(const QString &body, const QString &title, int max = 90);

// Wiki links between live notes, computed locally.
struct LinkIndex {
    QHash<QString, QStringList> byTitle;      // normalized title -> note ids, newest first
    QHash<QString, QSet<QString>> backlinks;  // note id -> ids linking to it
    QString resolve(const QString &title) const;
};
LinkIndex buildLinkIndex(const QList<const Note *> &notes);

struct ImageLink {
    QString url, alt;
    bool legacy = false;
};
// Parse a whole image block with the same CommonMark parser as the preview.
// Also recognize old imports containing a standalone self-labelled image link.
QString imageDefinitions(const QString &body);
ImageLink standaloneImage(const QString &line, const QString &definitions = QString());
QString displayMarkdown(const QString &body); // display-only legacy compatibility

// Line-level fence tracking, used by the editor highlighter and toggles.
bool isFenceLine(const QString &line);
// 0 outside code; otherwise encodes marker and minimum closing length.
// Returns true only for an opening or matching closing delimiter.
bool advanceFence(const QString &line, int &state);

// GFM tables: a header row, a delimiter row (|---|:-:|), then body rows.
bool isTableRow(const QString &line);       // has a cell separator
bool isTableDelimiter(const QString &line); // |---|:-:|---:|
QStringList tableCells(const QString &line); // trimmed cell texts, escaped \| kept as |
// Per column: Qt::AlignLeft, Qt::AlignHCenter or Qt::AlignRight.
QList<int> tableAlignments(const QString &delimiter);
// Inline Markdown reduced to its visible text (for table cells and the like).
QString plainInline(const QString &text);
// A table that directly follows a paragraph line gets a blank line before it,
// as GitHub renders it (md4c alone keeps it in the paragraph).
QString separateTables(const QString &body);

} // namespace md

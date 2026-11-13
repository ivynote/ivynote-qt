#include "core/markdown.h"

#include "core/models.h"

#include <QRegularExpression>
#include <algorithm>
#include <md4c.h>
#include <md4c-html.h>
#include <QTextDocumentFragment>
#include <QUrl>

namespace md {

namespace {

const QRegularExpression &wikiRe() {
    static const QRegularExpression re(QStringLiteral(R"(\[\[([^\[\]\n]+?)\]\])"));
    return re;
}
const QRegularExpression &noteRe() {
    static const QRegularExpression re(QStringLiteral(R"(ivy://note/([A-Za-z0-9_-]{8,64}))"));
    return re;
}
const QRegularExpression &resRe() {
    static const QRegularExpression re(QStringLiteral(R"(ivy://res/([A-Za-z0-9_-]{8,64}))"));
    return re;
}
const QRegularExpression &fenceRe() {
    static const QRegularExpression re(QStringLiteral(R"(^\s*(```|~~~)[\s\S]*?^\s*(```|~~~)\s*$)"),
                                       QRegularExpression::MultilineOption);
    return re;
}
const QRegularExpression &inlineCodeRe() {
    static const QRegularExpression re(QStringLiteral("`[^`\\n]*`"));
    return re;
}

QStringList distinctCaptures(const QString &text, const QRegularExpression &re) {
    QStringList out;
    QSet<QString> seen;
    for (auto it = re.globalMatch(text); it.hasNext();) {
        const QString id = it.next().captured(1);
        if (!seen.contains(id)) {
            seen.insert(id);
            out << id;
        }
    }
    return out;
}

bool isCjk(char32_t c) {
    return (c >= 0x3040 && c <= 0x30FF) || (c >= 0x3400 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF);
}

} // namespace

QString normalizeTitle(const QString &t) {
    return t.simplified().toLower();
}

QString stripCode(const QString &body) {
    QStringList kept;
    int fence = 0;
    for (const QString &line : body.split(u'\n')) {
        if (advanceFence(line, fence) || fence) kept << QString();
        else kept << line;
    }
    QString s = kept.join(u'\n');
    s.remove(inlineCodeRe());
    return s;
}

WikiLink parseWiki(const QString &inner) {
    WikiLink w;
    QString target = inner;
    const int bar = target.indexOf(u'|');
    if (bar >= 0) {
        w.alias = target.mid(bar + 1).trimmed();
        target = target.left(bar);
    }
    const int hash = target.indexOf(u'#');
    if (hash >= 0) {
        w.heading = target.mid(hash + 1).trimmed();
        target = target.left(hash);
    }
    w.target = target.trimmed();
    return w;
}

QStringList wikiTargets(const QString &body) {
    QStringList out;
    QSet<QString> seen;
    const QString text = stripCode(body);
    for (auto it = wikiRe().globalMatch(text); it.hasNext();) {
        const QString key = normalizeTitle(parseWiki(it.next().captured(1)).target);
        if (!key.isEmpty() && !seen.contains(key)) {
            seen.insert(key);
            out << key;
        }
    }
    return out;
}

QStringList noteRefs(const QString &body) { return distinctCaptures(stripCode(body), noteRe()); }
QStringList resourceRefs(const QString &body) { return distinctCaptures(body, resRe()); }

QString excerpt(const QString &mdText, int max) {
    static const QRegularExpression image(QStringLiteral(R"(!\[[^\]]*\]\([^)]*\))"));
    static const QRegularExpression wikiAlias(QStringLiteral(R"(\[\[([^\]|]+)\|([^\]]+)\]\])"));
    static const QRegularExpression wiki(QStringLiteral(R"(\[\[([^\]]+)\]\])"));
    static const QRegularExpression link(QStringLiteral(R"(\[([^\]]*)\]\([^)]*\))"));
    static const QRegularExpression lead(QStringLiteral(R"(^\s{0,3}(#{1,6}\s+|>\s?|[-*+]\s+\[[ xX]\]\s+|[-*+]\s+|\d+[.)]\s+))"),
                                         QRegularExpression::MultilineOption);
    static const QRegularExpression emphasis(QStringLiteral(R"((\*\*|__|~~|==|`))"));
    static const QRegularExpression html(QStringLiteral("<[^>]+>"));
    QString t = mdText;
    t.replace(fenceRe(), QStringLiteral(" "));
    t.remove(image);
    t.replace(wikiAlias, QStringLiteral("\\2"));
    t.replace(wiki, QStringLiteral("\\1"));
    t.replace(link, QStringLiteral("\\1"));
    t.remove(lead);
    t.remove(emphasis);
    t.remove(html);
    t = t.simplified();
    if (max >= 0 && t.size() > max) return t.left(max).trimmed() + QStringLiteral("…");
    return t;
}

int wordCount(const QString &mdText) {
    const QString text = excerpt(mdText, -1);
    int count = 0;
    bool inWord = false;
    for (char32_t c : text.toUcs4()) {
        if (isCjk(c)) {
            count++;
            inWord = false;
        } else if (QChar::isLetterOrNumber(c)) {
            if (!inWord) count++;
            inWord = true;
        } else {
            inWord = false;
        }
    }
    return count;
}

QString slug(const QString &text) {
    static const QRegularExpression tags(QStringLiteral("<[^>]+>"));
    static const QRegularExpression spaces(QStringLiteral("\\s+"));
    QString s = text.toLower().trimmed();
    s.remove(tags);
    QString kept;
    for (QChar c : s)
        if (c.isLetterOrNumber() || c.isSpace() || c == u'-') kept += c;
    kept.replace(spaces, QStringLiteral("-"));
    return kept;
}

QString imageDefinitions(const QString &body) {
    static const QRegularExpression definition(QStringLiteral(R"(^ {0,3}\[[^\]\n]+\]:[^\n]*(?:\n[ \t]+[^\n]+)?)"), QRegularExpression::MultilineOption);
    QStringList lines;
    const QString outsideCode = stripCode(body);
    for (auto it = definition.globalMatch(outsideCode); it.hasNext();) lines << it.next().captured(0);
    return lines.join(u'\n');
}

ImageLink standaloneImage(const QString &line, const QString &definitions) {
    const QString trimmed = line.trimmed();
    if (!trimmed.startsWith(QLatin1String("![")) && !trimmed.startsWith(u'[') && !trimmed.startsWith(u'#')) return {};
    if (!trimmed.contains(u'[')) return {};
    const QByteArray input = (line + QStringLiteral("\n\n") + definitions).toUtf8();
    QByteArray output;
    md_html(input.constData(), MD_SIZE(input.size()), [](const MD_CHAR *s, MD_SIZE n, void *p) {
        static_cast<QByteArray *>(p)->append(s, n);
    }, &output, MD_DIALECT_GITHUB | MD_FLAG_NOHTML, 0);
    const QString html = QString::fromUtf8(output).trimmed();
    auto decode = [](const QString &s) { return QTextDocumentFragment::fromHtml(s).toPlainText(); };
    static const QRegularExpression image(QStringLiteral(R"RE(^(?:<p>|<h[1-6]>)<img src="([^"]*)" alt="([^"]*)"[^>]*>(?:</p>|</h[1-6]>)$)RE"));
    if (const auto m = image.match(html); m.hasMatch()) return {decode(m.captured(1)), decode(m.captured(2)), false};
    static const QRegularExpression link(QStringLiteral(R"RE(^(?:<p>|<h[1-6]>)<a href="([^"]*)"[^>]*>([^<>]*)</a>(?:</p>|</h[1-6]>)$)RE"));
    const auto m = link.match(html);
    if (!m.hasMatch()) return {};
    const QString url = decode(m.captured(1)), label = decode(m.captured(2));
    const QUrl u(url);
    static const QRegularExpression extension(QStringLiteral(R"(\.(?:png|jpe?g|gif|webp|bmp|svg|avif)$)"), QRegularExpression::CaseInsensitiveOption);
    if ((u.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0 || u.scheme().compare(QLatin1String("http"), Qt::CaseInsensitive) == 0) &&
        label == url && extension.match(u.path()).hasMatch()) return {url, label, true};
    return {};
}

QString displayMarkdown(const QString &body) {
    QStringList lines = body.split(u'\n');
    int fence = 0;
    for (QString &line : lines) {
        if (advanceFence(line, fence) || fence) continue;
        if (!line.contains(QLatin1String("](http"), Qt::CaseInsensitive) && !line.contains(QLatin1String("](<http"), Qt::CaseInsensitive)) continue;
        if (const ImageLink image = standaloneImage(line); image.legacy) {
            // Angle destinations preserve parentheses. md4c escaped the source;
            // use its resolved URL, escaping angle brackets for Markdown.
            QString url = image.url;
            url.replace(u'<', QStringLiteral("%3C")).replace(u'>', QStringLiteral("%3E"));
            line = QStringLiteral("![](<%1>)").arg(url);
        }
    }
    return lines.join(u'\n');
}

bool advanceFence(const QString &line, int &state) {
    static const QRegularExpression fence(QStringLiteral(R"(^ {0,3}(`{3,}|~{3,})(.*)$)"));
    const auto m = fence.match(line);
    if (!m.hasMatch()) return false;
    const QString run = m.captured(1), tail = m.captured(2);
    const bool tilde = run.front() == u'~';
    if (state) {
        if (tilde != bool(state & 1) || run.size() < (state >> 1) || !tail.trimmed().isEmpty()) return false;
        state = 0;
    } else {
        if (!tilde && tail.contains(u'`')) return false;
        state = (int(run.size()) << 1) | int(tilde);
    }
    return true;
}

bool isFenceLine(const QString &line) {
    int state = 0;
    return advanceFence(line, state);
}

QList<Heading> outline(const QString &body) {
    static const QRegularExpression re(QStringLiteral(R"(^(#{1,6})\s+(.+?)\s*#*\s*$)"));
    QList<Heading> out;
    int fence = 0;
    const QStringList lines = body.split(u'\n');
    for (int i = 0; i < lines.size(); ++i) {
        if (advanceFence(lines[i], fence)) {
            continue;
        }
        if (fence || standaloneImage(lines[i]).legacy) continue;
        const auto m = re.match(lines[i]);
        if (m.hasMatch()) out << Heading{int(m.capturedLength(1)), m.captured(2), i};
    }
    return out;
}

QString toggleTask(const QString &body, int index) {
    if (index < 0) return body;
    struct Tasks { int wanted; int count = 0; qsizetype offset = -1; } tasks{index};
    MD_PARSER parser{};
    parser.flags = MD_DIALECT_GITHUB | MD_FLAG_NOHTML | MD_FLAG_WIKILINKS;
    parser.enter_block = [](MD_BLOCKTYPE type, void *detail, void *data) -> int {
        auto &tasks = *static_cast<Tasks *>(data);
        if (type == MD_BLOCK_LI) {
            const auto &li = *static_cast<MD_BLOCK_LI_DETAIL *>(detail);
            if (li.is_task && tasks.count++ == tasks.wanted) tasks.offset = li.task_mark_offset;
        }
        return 0;
    };
    parser.leave_block = [](MD_BLOCKTYPE, void *, void *) { return 0; };
    parser.enter_span = [](MD_SPANTYPE, void *, void *) { return 0; };
    parser.leave_span = [](MD_SPANTYPE, void *, void *) { return 0; };
    parser.text = [](MD_TEXTTYPE, const MD_CHAR *, MD_SIZE, void *) { return 0; };
    QByteArray source = body.toUtf8();
    if (md_parse(source.constData(), MD_SIZE(source.size()), &parser, &tasks) != 0 || tasks.offset < 0 || tasks.offset >= source.size()) return body;
    source[tasks.offset] = source[tasks.offset] == ' ' ? 'x' : ' ';
    return QString::fromUtf8(source);
}

QString linkContext(const QString &body, const QString &title, int max) {
    static const QRegularExpression lead(QStringLiteral(R"(^[\s>#*\-+\d.)\[\]x]+)"));
    const QString key = normalizeTitle(title);
    for (const QString &line : body.split(u'\n')) {
        for (auto it = wikiRe().globalMatch(line); it.hasNext();) {
            if (normalizeTitle(parseWiki(it.next().captured(1)).target) != key) continue;
            QString text;
            int last = 0;
            for (auto jt = wikiRe().globalMatch(line); jt.hasNext();) {
                const auto m = jt.next();
                text += line.mid(last, m.capturedStart() - last);
                const WikiLink w = parseWiki(m.captured(1));
                text += w.alias.isEmpty() ? w.target : w.alias;
                last = m.capturedEnd();
            }
            text += line.mid(last);
            text.remove(lead);
            text = text.trimmed();
            return text.size() > max ? text.left(max) + QStringLiteral("…") : text;
        }
    }
    return {};
}

QString LinkIndex::resolve(const QString &title) const {
    const auto it = byTitle.constFind(normalizeTitle(title));
    return it == byTitle.constEnd() || it->isEmpty() ? QString() : it->first();
}

LinkIndex buildLinkIndex(const QList<const Note *> &notes) {
    LinkIndex idx;
    QList<const Note *> live;
    QSet<QString> ids;
    for (const Note *n : notes) {
        if (n->trashed() || n->deleted) continue;
        live << n;
        ids.insert(n->id);
    }
    QList<const Note *> sorted = live;
    std::sort(sorted.begin(), sorted.end(), [](const Note *a, const Note *b) { return a->updatedAt > b->updatedAt; });
    for (const Note *n : sorted) {
        const QString k = normalizeTitle(n->title);
        if (!k.isEmpty()) idx.byTitle[k] << n->id;
    }
    for (const Note *n : live) {
        QSet<QString> targets;
        for (const QString &t : wikiTargets(n->body)) {
            const QString hit = idx.resolve(t);
            if (!hit.isEmpty() && hit != n->id) targets.insert(hit);
        }
        for (const QString &id : noteRefs(n->body))
            if (ids.contains(id) && id != n->id) targets.insert(id);
        for (const QString &t : targets) idx.backlinks[t].insert(n->id);
    }
    return idx;
}

// --- tables ---

namespace {

// Splits on unescaped pipes that are not inside a code span.
QStringList splitRow(const QString &line) {
    QString t = line.trimmed();
    QStringList cells;
    QString cur;
    bool code = false, trailingSeparator = false;
    for (qsizetype i = 0; i < t.size(); ++i) {
        const QChar c = t[i];
        if (c == u'\\' && i + 1 < t.size() && t[i + 1] == u'\\') {
            cur += QStringLiteral("\\\\");
            ++i;
            continue;
        }
        if (c == u'\\' && i + 1 < t.size() && t[i + 1] == u'|') {
            cur += u'|';
            ++i;
            continue;
        }
        if (c == u'`') code = !code;
        if (c == u'|' && !code) {
            trailingSeparator = i == t.size() - 1;
            cells << cur;
            cur.clear();
            continue;
        }
        cur += c;
    }
    cells << cur;
    // Leading and trailing pipes are optional.
    if (t.startsWith(u'|') && !cells.isEmpty()) cells.removeFirst();
    if (trailingSeparator && !cells.isEmpty()) cells.removeLast();
    for (QString &c : cells) c = c.trimmed();
    return cells;
}

} // namespace

bool isTableRow(const QString &line) {
    const QString t = line.trimmed();
    if (t.isEmpty() || !t.contains(u'|')) return false;
    // Lines inside a quote or indented as code are not table rows here.
    return !t.startsWith(u'>') && !line.startsWith(QLatin1String("    ")) && !line.startsWith(u'\t');
}

bool isTableDelimiter(const QString &line) {
    static const QRegularExpression re(QStringLiteral(R"(^\s*\|?\s*:?-+:?\s*(\|\s*:?-+:?\s*)*\|?\s*$)"));
    return line.contains(u'|') && line.contains(u'-') && re.match(line).hasMatch() && !line.startsWith(QLatin1String("    "));
}

QStringList tableCells(const QString &line) { return splitRow(line); }

QList<int> tableAlignments(const QString &delimiter) {
    QList<int> out;
    for (const QString &c : splitRow(delimiter)) {
        const bool l = c.startsWith(u':'), r = c.endsWith(u':');
        out << (l && r ? int(Qt::AlignHCenter) : r ? int(Qt::AlignRight) : int(Qt::AlignLeft));
    }
    return out;
}

QString plainInline(const QString &text) {
    static const QRegularExpression wiki(QStringLiteral(R"(\[\[([^\]|]*\|)?([^\]]+)\]\])"));
    static const QRegularExpression link(QStringLiteral(R"(!?\[([^\]]*)\]\([^)]*\))"));
    static const QRegularExpression marks(QStringLiteral(R"((\*\*|__|~~|==|`))"));
    static const QRegularExpression em(QStringLiteral(R"((?<![\w*])\*(?=\S)(.+?)(?<=\S)\*(?![\w*])|(?<![\w_])_(?=\S)(.+?)(?<=\S)_(?![\w_]))"));
    QString s = text;
    s.replace(wiki, QStringLiteral("\\2"));
    s.replace(link, QStringLiteral("\\1"));
    s.replace(marks, QString());
    s.replace(em, QStringLiteral("\\1\\2"));
    return s;
}

Table parseTable(const QString &source) {
    const QStringList lines = source.split(u'\n');
    if (lines.size() < 2 || !isTableRow(lines[0]) || !isTableDelimiter(lines[1])) return {};
    Table table;
    table.align = tableAlignments(lines[1]);
    const int cols = int(tableCells(lines[0]).size());
    if (!cols || table.align.size() != cols) return {};
    for (int i = 0; i < lines.size(); ++i) {
        if (i == 1) continue;
        if (lines[i].trimmed().isEmpty() && i == lines.size() - 1) break;
        if (!isTableRow(lines[i])) return {};
        QStringList row = tableCells(lines[i]);
        while (row.size() < cols) row << QString();
        row = row.mid(0, cols); // GFM discards cells beyond the header width
        table.rows << row;
    }
    return table;
}

QString formatTable(const Table &table) {
    if (!table.valid()) return {};
    const int cols = int(table.align.size());
    QList<QStringList> rows;
    QList<int> widths(cols, 3);
    for (const QStringList &raw : table.rows) {
        QStringList row;
        for (int col = 0; col < cols; ++col) {
            QString value = raw.value(col).trimmed();
            value.replace(u'\r', u' ').replace(u'\n', u' ');
            // Escape cell separators, preserving preceding literal backslashes.
            value.replace(u'|', QStringLiteral("\\|"));
            widths[col] = std::max(widths[col], int(value.size()));
            row << value;
        }
        rows << row;
    }
    QStringList out;
    for (int row = 0; row < rows.size(); ++row) {
        QStringList cells;
        for (int col = 0; col < cols; ++col) cells << rows[row][col].leftJustified(widths[col], u' ');
        out << QStringLiteral("| ") + cells.join(QStringLiteral(" | ")) + QStringLiteral(" |");
        if (row == 0) {
            QStringList separator;
            for (int col = 0; col < cols; ++col) {
                QString value(widths[col], u'-');
                if (table.align[col] == Qt::AlignHCenter) value.front() = value.back() = u':';
                else if (table.align[col] == Qt::AlignRight) value.back() = u':';
                separator << value;
            }
            out << QStringLiteral("| ") + separator.join(QStringLiteral(" | ")) + QStringLiteral(" |");
        }
    }
    return out.join(u'\n');
}

QString separateTables(const QString &body) {
    if (!body.contains(u'|')) return body;
    const QStringList lines = body.split(u'\n');
    QStringList out;
    out.reserve(lines.size() + 4);
    int fence = 0;
    bool table = false;
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QString &line = lines[i];
        if (advanceFence(line, fence)) {
            table = false;
        }
        if (!fence) {
            if (table && !isTableRow(line)) table = false;
            if (!table && isTableRow(line) && i + 1 < lines.size() && isTableDelimiter(lines[i + 1]) && !isTableDelimiter(line)) {
                table = true;
                const QString prev = i > 0 ? lines[i - 1] : QString();
                if (!prev.trimmed().isEmpty() && !isFenceLine(prev)) out << QString();
            }
        }
        out << line;
    }
    return out.join(u'\n');
}

} // namespace md

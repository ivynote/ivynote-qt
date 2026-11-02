#include "core/markdown.h"

#include "core/models.h"

#include <QRegularExpression>
#include <algorithm>

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
    QString s = body;
    s.remove(fenceRe());
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

bool isFenceLine(const QString &line) {
    const QString t = line.trimmed();
    return t.startsWith(QLatin1String("```")) || t.startsWith(QLatin1String("~~~"));
}

QList<Heading> outline(const QString &body) {
    static const QRegularExpression re(QStringLiteral(R"(^(#{1,6})\s+(.+?)\s*#*\s*$)"));
    QList<Heading> out;
    bool fence = false;
    const QStringList lines = body.split(u'\n');
    for (int i = 0; i < lines.size(); ++i) {
        if (isFenceLine(lines[i])) {
            fence = !fence;
            continue;
        }
        if (fence) continue;
        const auto m = re.match(lines[i]);
        if (m.hasMatch()) out << Heading{int(m.capturedLength(1)), m.captured(2), i};
    }
    return out;
}

QString toggleTask(const QString &body, int index) {
    static const QRegularExpression re(QStringLiteral(R"(^(\s*(?:[-*+]|\d+[.)])\s+\[)([ xX])(\].*)$)"));
    QStringList lines = body.split(u'\n');
    bool fence = false;
    int n = 0;
    for (QString &line : lines) {
        if (isFenceLine(line)) {
            fence = !fence;
            continue;
        }
        if (fence) continue;
        const auto m = re.match(line);
        if (!m.hasMatch()) continue;
        if (n++ == index) {
            const bool done = m.captured(2) != QLatin1String(" ");
            line = m.captured(1) + (done ? QStringLiteral(" ") : QStringLiteral("x")) + m.captured(3);
            return lines.join(u'\n');
        }
    }
    return body;
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
    bool code = false;
    for (qsizetype i = 0; i < t.size(); ++i) {
        const QChar c = t[i];
        if (c == u'\\' && i + 1 < t.size() && t[i + 1] == u'|') {
            cur += u'|';
            ++i;
            continue;
        }
        if (c == u'`') code = !code;
        if (c == u'|' && !code) {
            cells << cur;
            cur.clear();
            continue;
        }
        cur += c;
    }
    cells << cur;
    // Leading and trailing pipes are optional.
    if (t.startsWith(u'|') && !cells.isEmpty()) cells.removeFirst();
    if (t.endsWith(u'|') && !t.endsWith(QLatin1String("\\|")) && !cells.isEmpty()) cells.removeLast();
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

QString separateTables(const QString &body) {
    if (!body.contains(u'|')) return body;
    const QStringList lines = body.split(u'\n');
    QStringList out;
    out.reserve(lines.size() + 4);
    bool fence = false, table = false;
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QString &line = lines[i];
        if (isFenceLine(line)) {
            fence = !fence;
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

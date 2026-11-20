#include "ui/render.h"

#include "core/markdown.h"

#include <QDate>
#include <QRegularExpression>
#include <QUrl>
#include <QTextDocumentFragment>
#include <md4c-html.h>

namespace {

void collect(const MD_CHAR *text, MD_SIZE size, void *user) {
    static_cast<QByteArray *>(user)->append(text, qsizetype(size));
}

QString escape(const QString &s) { return s.toHtmlEscaped(); }

QString unescape(QString s) {
    s.replace(QLatin1String("&quot;"), QLatin1String("\""));
    s.replace(QLatin1String("&lt;"), QLatin1String("<"));
    s.replace(QLatin1String("&gt;"), QLatin1String(">"));
    s.replace(QLatin1String("&amp;"), QLatin1String("&"));
    return s;
}

} // namespace

namespace {

// QTextBrowser has no flex, padding on blocks or left borders, so the
// pieces that need them become small tables.

const QRegularExpression &listTag() {
    static const QRegularExpression re(QStringLiteral(R"RE(<(/?)(ul|ol|li)((?: [^>]*)?)>)RE"));
    return re;
}

QString parseList(const QString &h, qsizetype &i, const QString &name, const QString &open);

// Copies HTML up to the closing tag `stop` (consumed), rewriting lists.
QString parseFlow(const QString &h, qsizetype &i, const QString &stop) {
    QString out;
    while (i < h.size()) {
        const auto m = listTag().match(h, i);
        if (!m.hasMatch()) break;
        out += h.mid(i, m.capturedStart() - i);
        i = m.capturedEnd();
        const bool close = !m.captured(1).isEmpty();
        const QString name = m.captured(2);
        if (close && name == stop) return out;
        if (close || name == QLatin1String("li")) {
            out += m.captured(0);
            continue;
        }
        out += parseList(h, i, name, m.captured(0));
    }
    out += h.mid(i);
    i = h.size();
    return out;
}

QString parseList(const QString &h, qsizetype &i, const QString &name, const QString &open) {
    struct Item {
        bool task = false;
        QString body;
    };
    QList<Item> items;
    while (i < h.size()) {
        const auto m = listTag().match(h, i);
        if (!m.hasMatch()) {
            i = h.size();
            break;
        }
        i = m.capturedEnd();
        const bool close = !m.captured(1).isEmpty();
        if (close && m.captured(2) == name) break;
        if (!close && m.captured(2) == QLatin1String("li")) {
            Item it;
            it.task = m.captured(3).contains(QLatin1String("task-list-item"));
            it.body = parseFlow(h, i, QStringLiteral("li"));
            items << it;
        }
    }
    // Every list becomes rows: the marker (checkbox, faint bullet or number)
    // in a narrow column, the text hanging beside it.
    static const QRegularExpression box(QStringLiteral(R"RE(^\s*((?:<a href="ivy-task:\d+" class="task">|<span class="task">)<img src="ivy-check:([01])"[^>]*>(?:</a>|</span>)))RE"));
    static const QRegularExpression start(QStringLiteral(R"RE(start="(\d+)")RE"));
    int number = 1;
    if (const auto sm = start.match(open); sm.hasMatch()) number = sm.captured(1).toInt();
    QString out = QStringLiteral("<table class=\"t\" width=\"100%\" cellspacing=\"0\" cellpadding=\"0\">");
    for (const Item &it : std::as_const(items)) {
        QString mark, body = it.body;
        bool done = false;
        if (const auto bm = box.match(body); it.task && bm.hasMatch()) {
            mark = bm.captured(1);
            done = bm.captured(2) == QLatin1String("1");
            body = body.mid(bm.capturedEnd());
        } else {
            mark = name == QLatin1String("ol") ? QStringLiteral("%1.").arg(number) : QStringLiteral("•");
        }
        number++;
        const bool box = mark.startsWith(u'<');
        out += QStringLiteral("<tr><td class=\"%1\" width=\"%2\" valign=\"top\">%3</td><td class=\"%4\" valign=\"top\">%5</td></tr>")
                   .arg(box ? QStringLiteral("tb") : QStringLiteral("tm"), name == QLatin1String("ol") && !box ? QStringLiteral("28") : QStringLiteral("24"), mark,
                        done ? QStringLiteral("td") : QStringLiteral("ti"), body.trimmed());
    }
    return out + QStringLiteral("</table>\n");
}

QString layoutForView(QString html) {
    qsizetype i = 0;
    html = parseFlow(html, i, QString());
    // Code blocks on a padded panel, without the blank line md4c ends them with.
    html.replace(QLatin1String("\n</code></pre>"), QLatin1String("</code></pre>"));
    static const QRegularExpression pre(QStringLiteral(R"RE(<pre>(.*?)</pre>)RE"), QRegularExpression::DotMatchesEverythingOption);
    html.replace(pre, QStringLiteral("<table class=\"c\" width=\"100%\" cellspacing=\"0\" cellpadding=\"0\"><tr><td class=\"c\"><pre>\\1</pre></td></tr></table>\n"));
    // Quotes with the ivy bar on the left, innermost first.
    static const QRegularExpression quote(QStringLiteral(R"RE(<blockquote>((?:(?!<blockquote>).)*?)</blockquote>)RE"),
                                          QRegularExpression::DotMatchesEverythingOption);
    while (html.contains(QLatin1String("<blockquote>"))) {
        const QString before = html;
        html.replace(quote, QStringLiteral("<table class=\"q\" width=\"100%\" cellspacing=\"0\" cellpadding=\"0\"><tr><td class=\"qb\" width=\"2\"></td>"
                                           "<td class=\"qt\">\\1</td></tr></table>\n"));
        if (html == before) break;
    }
    // A thin full-width line, as the live editor draws it.
    html.replace(QLatin1String("<hr>"), QLatin1String("<table class=\"hr\" width=\"100%\" cellspacing=\"0\" cellpadding=\"0\"><tr><td class=\"hr\"></td></tr></table>"));
    return html;
}

} // namespace

bool isSafeExternalUrl(const QString &url) {
    const QString scheme = QUrl(url).scheme().toLower();
    return scheme == QLatin1String("http") || scheme == QLatin1String("https") || scheme == QLatin1String("mailto");
}

QString renderMarkdown(const QString &markdown, const RenderOptions &opts) {
    const QByteArray src = md::separateTables(md::displayMarkdown(markdown)).toUtf8();
    QByteArray out;
    const unsigned flags = MD_DIALECT_GITHUB | MD_FLAG_NOHTML | MD_FLAG_WIKILINKS;
    md_html(src.constData(), MD_SIZE(src.size()), collect, &out, flags, 0);
    QString html = QString::fromUtf8(out);
    if (opts.forView) {
        // Mark the cells of Markdown tables before other tables are made.
        static const QRegularExpression cell(QStringLiteral(R"RE(<(td|th)((?: [^>]*)?)>)RE"));
        html.replace(cell, QStringLiteral("<\\1 class=\"g\"\\2>"));
        html.replace(QLatin1String("<table>"), QLatin1String("<table class=\"g\" width=\"100%\" cellspacing=\"0\" cellpadding=\"0\">"));
    }

    // Wiki links: <x-wikilink data-target="T">label</x-wikilink>
    static const QRegularExpression wiki(QStringLiteral(R"RE(<x-wikilink data-target="([^"]*)">(.*?)</x-wikilink>)RE"),
                                         QRegularExpression::DotMatchesEverythingOption);
    QString result;
    qsizetype last = 0;
    for (auto it = wiki.globalMatch(html); it.hasNext();) {
        const auto m = it.next();
        result += html.mid(last, m.capturedStart() - last);
        const QString inner = unescape(m.captured(1));
        const md::WikiLink w = md::parseWiki(inner);
        QString label = m.captured(2);
        if (label.trimmed().isEmpty() || unescape(label) == inner) label = escape(w.alias.isEmpty() ? (w.heading.isEmpty() ? w.target : w.target + QStringLiteral(" › ") + w.heading) : w.alias);
        const bool exists = !opts.resolveTitle || !opts.resolveTitle(w.target).isEmpty();
        const QString href = QStringLiteral("ivy-wiki:") + QString::fromLatin1(QUrl::toPercentEncoding(inner));
        result += QStringLiteral("<a href=\"%1\" class=\"%2\">%3</a>").arg(href, exists ? QStringLiteral("wiki") : QStringLiteral("wiki missing"), label);
        last = m.capturedEnd();
    }
    result += html.mid(last);
    html = result;

    // Only safe link targets survive; everything else loses its href.
    static const QRegularExpression href(QStringLiteral(R"RE(<a href="([^"]*)")RE"));
    result.clear();
    last = 0;
    for (auto it = href.globalMatch(html); it.hasNext();) {
        const auto m = it.next();
        result += html.mid(last, m.capturedStart() - last);
        const QString url = unescape(m.captured(1));
        const bool ok = isSafeExternalUrl(url) || url.startsWith(QLatin1String("ivy://")) || url.startsWith(QLatin1String("ivy-wiki:")) ||
                        url.startsWith(u'#');
        result += ok ? m.captured(0) : QStringLiteral("<a");
        last = m.capturedEnd();
    }
    result += html.mid(last);
    html = result;

    // Attachments: images stay images (loaded by the browser's resource hook);
    // videos, audio and files become cards.
    static const QRegularExpression img(QStringLiteral(R"RE(<img src="([^"]*)" alt="([^"]*)"[^>]*>)RE"));
    static const QRegularExpression resId(QStringLiteral(R"RE(^ivy://res/([A-Za-z0-9_-]{8,64})$)RE"));
    result.clear();
    last = 0;
    for (auto it = img.globalMatch(html); it.hasNext();) {
        const auto m = it.next();
        result += html.mid(last, m.capturedStart() - last);
        const QString url = unescape(m.captured(1));
        const auto rm = resId.match(url);
        if (rm.hasMatch()) {
            const QString id = rm.captured(1);
            const QString kind = opts.resourceKind ? opts.resourceKind(id) : QString();
            if (kind.isEmpty() || kind == QLatin1String("image")) {
                result += QStringLiteral("<img src=\"ivy://res/%1\" alt=\"%2\">").arg(id, m.captured(2));
            } else {
                const QString icon = kind == QLatin1String("video") ? QStringLiteral("🎬") : kind == QLatin1String("audio") ? QStringLiteral("🎵") : QStringLiteral("📎");
                QString name = m.captured(2);
                if (name.isEmpty() && opts.resourceName) name = escape(opts.resourceName(id));
                result += QStringLiteral("<a href=\"ivy://res/%1\" class=\"file\">%2 %3</a>").arg(id, icon, name);
            }
        } else if (opts.remoteImages && (QUrl(url).scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0 || QUrl(url).scheme().compare(QLatin1String("http"), Qt::CaseInsensitive) == 0)) {
            result += QStringLiteral("<img src=\"%1\" alt=\"%2\">").arg(m.captured(1), m.captured(2));
        } else if (isSafeExternalUrl(url)) {
            // Without remoteImages nothing is fetched; show a link.
            result += QStringLiteral("<a href=\"%1\">🖼 %2</a>").arg(m.captured(1), m.captured(2).isEmpty() ? m.captured(1) : m.captured(2));
        } else {
            result += m.captured(2);
        }
        last = m.capturedEnd();
    }
    result += html.mid(last);
    html = result;

    // File links written as [name](ivy://res/id) get the paperclip.
    static const QRegularExpression resLink(QStringLiteral(R"RE(<a href="ivy://res/([A-Za-z0-9_-]{8,64})">)RE"));
    html.replace(resLink, QStringLiteral("<a href=\"ivy://res/\\1\" class=\"file\">📎 "));

    // Task boxes become clickable glyphs (QTextBrowser has no form inputs).
    static const QRegularExpression task(QStringLiteral(R"RE(<input type="checkbox" class="task-list-item-checkbox"[^>]*?( checked(?:="checked")?)?>)RE"));
    result.clear();
    last = 0;
    int n = 0;
    for (auto it = task.globalMatch(html); it.hasNext();) {
        const auto m = it.next();
        result += html.mid(last, m.capturedStart() - last);
        const bool done = !m.captured(1).isEmpty();
        // The view draws the box as an image, like the editor does.
        const QString box = opts.forView ? QStringLiteral("<img src=\"ivy-check:%1\" width=\"18\" height=\"20\">").arg(done ? 1 : 0)
                                         : (done ? QStringLiteral("☑") : QStringLiteral("☐"));
        if (opts.interactiveTasks) result += QStringLiteral("<a href=\"ivy-task:%1\" class=\"task\">%2</a>").arg(n).arg(box);
        else result += QStringLiteral("<span class=\"task\">%1</span>").arg(box);
        n++;
        last = m.capturedEnd();
    }
    result += html.mid(last);
    html = result;

    // Transform text nodes only: never touch attributes or code. In
    // particular, == in a URL must not turn into HTML inside its href.
    static const QRegularExpression mark(QStringLiteral(R"RE(==([^=\n<>][^=\n]*?)==)RE"));
    static const QRegularExpression dueRe(QStringLiteral(R"RE(📅️?\s*(\d{4}-\d{2}-\d{2})(?:[ T](\d{1,2}:\d{2}))?)RE"));
    static const QRegularExpression repeatRe(QStringLiteral(R"RE(🔁️?\s*([^📅✅⏳🛫➕⏫🔼🔽🔺⏬<\n]+?)(?=\s*(?:📅|✅|⏳|🛫|➕|⏫|🔼|🔽|🔺|⏬|$)))RE"));
    static const QRegularExpression doneRe(QStringLiteral(R"RE(✅️?\s*(\d{4}-\d{2}-\d{2}))RE"));
    static const QRegularExpression tags(QStringLiteral(R"RE(<[^>]*>)RE"));
    const QDate today = QDate::currentDate();
    auto styleTaskMarkers = [&](QString &node, bool inDoneTask) {
        if (!node.contains(QStringLiteral("📅")) && !node.contains(QStringLiteral("🔁")) && !node.contains(QStringLiteral("✅")))
            return;
        if (node.contains(QStringLiteral("📅"))) {
            QString out;
            qsizetype p = 0;
            for (auto it = dueRe.globalMatch(node); it.hasNext();) {
                const auto dm = it.next();
                out += node.mid(p, dm.capturedStart() - p);
                const QDate d = QDate::fromString(dm.captured(1), Qt::ISODate);
                QString cls = QStringLiteral("task-due");
                if (!inDoneTask && d.isValid()) {
                    if (d < today) cls = QStringLiteral("task-due-overdue");
                    else if (d == today) cls = QStringLiteral("task-due-today");
                }
                out += QStringLiteral("<span class=\"%1\">%2</span>").arg(cls, dm.captured(0).trimmed());
                p = dm.capturedEnd();
            }
            out += node.mid(p);
            node = out;
        }
        if (node.contains(QStringLiteral("🔁"))) {
            node.replace(repeatRe, QStringLiteral("<span class=\"task-repeat\">\\0</span>"));
        }
        if (node.contains(QStringLiteral("✅"))) {
            node.replace(doneRe, QStringLiteral("<span class=\"task-done\">\\0</span>"));
        }
    };
    result.clear();
    last = 0;
    int codeDepth = 0;
    bool inTaskLi = false;
    bool inDoneTask = false;
    for (auto it = tags.globalMatch(html); it.hasNext();) {
        const auto m = it.next();
        QString node = html.mid(last, m.capturedStart() - last);
        if (!codeDepth) {
            node.replace(mark, QStringLiteral("<span class=\"mark\">\\1</span>"));
            if (inTaskLi) styleTaskMarkers(node, inDoneTask);
        }
        const QString tag = m.captured(0);
        result += node + tag;
        if (tag.startsWith(QLatin1String("<code")) || tag == QLatin1String("<pre>")) ++codeDepth;
        else if (tag == QLatin1String("</code>") || tag == QLatin1String("</pre>")) codeDepth = std::max(0, codeDepth - 1);
        else if (tag.startsWith(QLatin1String("<li")) && tag.contains(QLatin1String("task-list-item"))) {
            inTaskLi = true;
            inDoneTask = false;
        } else if (tag == QLatin1String("</li>")) {
            inTaskLi = false;
            inDoneTask = false;
        } else if (inTaskLi && (tag.contains(QLatin1String("ivy-check:1")) || tag.contains(QLatin1String("ivy-check:d1")) || tag.contains(QLatin1String("ivy-check:l1")))) {
            inDoneTask = true;
        }
        last = m.capturedEnd();
    }
    QString tail = html.mid(last);
    if (!codeDepth) {
        tail.replace(mark, QStringLiteral("<span class=\"mark\">\\1</span>"));
        if (inTaskLi) styleTaskMarkers(tail, inDoneTask);
    }
    result += tail;

    // Heading anchors so [[Title#Heading]] and the outline can scroll there.
    static const QRegularExpression heading(QStringLiteral(R"RE(<h([1-6])>(.*?)</h\1>)RE"));
    html = result;
    result.clear();
    last = 0;
    for (auto it = heading.globalMatch(html); it.hasNext();) {
        const auto m = it.next();
        result += html.mid(last, m.capturedStart() - last);
        result += QStringLiteral("<h%1><a name=\"%2\"></a>%3</h%1>").arg(m.captured(1), md::slug(QTextDocumentFragment::fromHtml(m.captured(2)).toPlainText()), m.captured(2));
        last = m.capturedEnd();
    }
    result += html.mid(last);
    if (!opts.forView) return result;
    return layoutForView(result);
}


#include "ui/render.h"

#include "core/markdown.h"

#include <QRegularExpression>
#include <QUrl>
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

bool isSafeExternalUrl(const QString &url) {
    const QString scheme = QUrl(url).scheme().toLower();
    return scheme == QLatin1String("http") || scheme == QLatin1String("https") || scheme == QLatin1String("mailto");
}

QString renderMarkdown(const QString &markdown, const RenderOptions &opts) {
    const QByteArray src = markdown.toUtf8();
    QByteArray out;
    const unsigned flags = MD_DIALECT_GITHUB | MD_FLAG_NOHTML | MD_FLAG_WIKILINKS;
    md_html(src.constData(), MD_SIZE(src.size()), collect, &out, flags, 0);
    QString html = QString::fromUtf8(out);

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
        } else if (isSafeExternalUrl(url)) {
            // Remote images are not fetched (no tracking pixels); show a link.
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
    static const QRegularExpression task(QStringLiteral(R"RE(<input type="checkbox" class="task-list-item-checkbox"( checked="checked")?[^>]*>)RE"));
    result.clear();
    last = 0;
    int n = 0;
    for (auto it = task.globalMatch(html); it.hasNext();) {
        const auto m = it.next();
        result += html.mid(last, m.capturedStart() - last);
        const bool done = !m.captured(1).isEmpty();
        const QString box = done ? QStringLiteral("☑") : QStringLiteral("☐");
        if (opts.interactiveTasks) result += QStringLiteral("<a href=\"ivy-task:%1\" class=\"task\">%2</a>").arg(n).arg(box);
        else result += QStringLiteral("<span class=\"task\">%1</span>").arg(box);
        n++;
        last = m.capturedEnd();
    }
    result += html.mid(last);
    html = result;

    // ==highlight== outside code.
    static const QRegularExpression mark(QStringLiteral(R"RE(==([^=\n<>][^=\n]*?)==)RE"));
    static const QRegularExpression codeSplit(QStringLiteral(R"RE((<pre>.*?</pre>|<code>.*?</code>))RE"),
                                              QRegularExpression::DotMatchesEverythingOption);
    result.clear();
    last = 0;
    for (auto it = codeSplit.globalMatch(html); it.hasNext();) {
        const auto m = it.next();
        QString plain = html.mid(last, m.capturedStart() - last);
        plain.replace(mark, QStringLiteral("<span class=\"mark\">\\1</span>"));
        result += plain + m.captured(0);
        last = m.capturedEnd();
    }
    QString tail = html.mid(last);
    tail.replace(mark, QStringLiteral("<span class=\"mark\">\\1</span>"));
    result += tail;

    // Heading anchors so [[Title#Heading]] and the outline can scroll there.
    static const QRegularExpression heading(QStringLiteral(R"RE(<h([1-6])>(.*?)</h\1>)RE"));
    html = result;
    result.clear();
    last = 0;
    for (auto it = heading.globalMatch(html); it.hasNext();) {
        const auto m = it.next();
        result += html.mid(last, m.capturedStart() - last);
        result += QStringLiteral("<h%1><a name=\"%2\"></a>%3</h%1>").arg(m.captured(1), md::slug(m.captured(2)), m.captured(2));
        last = m.capturedEnd();
    }
    result += html.mid(last);
    return result;
}

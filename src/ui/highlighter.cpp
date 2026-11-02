#include "ui/highlighter.h"

#include "core/markdown.h"
#include "ui/theme.h"

#include <QFontMetricsF>
#include <QRegularExpression>
#include <QTextDocument>

namespace {

const QRegularExpression &re(const char *pattern) {
    // Small cache: patterns are compiled once.
    static QHash<const char *, QRegularExpression> cache;
    auto it = cache.find(pattern);
    if (it == cache.end()) it = cache.insert(pattern, QRegularExpression(QString::fromUtf8(pattern)));
    return *it;
}

QTextCharFormat fontFormat(const QFont &f) {
    QTextCharFormat fmt;
    fmt.setFont(f, QTextCharFormat::FontPropertiesAll);
    return fmt;
}

} // namespace

MarkdownHighlighter::MarkdownHighlighter(QTextDocument *doc) : QSyntaxHighlighter(doc) {
    connect(Theme::instance(), &Theme::changed, this, &QSyntaxHighlighter::rehighlight);
}

void MarkdownHighlighter::setMode(Mode m) {
    if (m_mode == m) return;
    m_mode = m;
    rehighlight();
}

void MarkdownHighlighter::setFontSize(int px) {
    if (m_px == px) return;
    m_px = px;
    rehighlight();
}

void MarkdownHighlighter::setActive(int first, int last) {
    if (first == m_first && last == m_last) return;
    const int oldFirst = m_first, oldLast = m_last;
    m_first = first;
    m_last = last;
    if (m_mode != Live) return;
    QTextDocument *doc = document();
    auto redo = [&](int a, int b) {
        if (a < 0) return;
        for (int i = a; i <= b; ++i) {
            const QTextBlock blk = doc->findBlockByNumber(i);
            if (blk.isValid()) rehighlightBlock(blk);
        }
    };
    redo(oldFirst, oldLast);
    redo(first, last);
}

void MarkdownHighlighter::hide(int start, int len) {
    if (len <= 0) return;
    QTextCharFormat f;
    f.setForeground(Qt::transparent);
    f.setProperty(QTextFormat::FontPixelSize, 1);
    f.setFontLetterSpacingType(QFont::PercentageSpacing);
    f.setFontLetterSpacing(1);
    setFormat(start, len, f);
}

QSize MarkdownHighlighter::displaySize(const QString &id) const {
    QSize natural = imageSize ? imageSize(id) : QSize();
    const int maxW = std::max(120, m_columnWidth - 8);
    const int maxH = std::max(160, int(m_viewportHeight * 0.6));
    if (!natural.isValid() || natural.isEmpty()) return QSize(std::min(maxW, 320), 140); // placeholder
    return natural.scaled(std::min(maxW, natural.width()), std::min(maxH, natural.height()), Qt::KeepAspectRatio);
}

void MarkdownHighlighter::highlightBlock(const QString &text) {
    const Tokens &k = Theme::t();
    const int num = currentBlock().blockNumber();
    const bool live = m_mode == Live;
    const bool active = !live || (num >= m_first && num <= m_last);
    const int prev = previousBlockState() < 0 ? 0 : previousBlockState();
    auto *info = new BlockInfo;
    info->active = active;
    setCurrentBlockUserData(info);
    const QFont mono = Theme::mono(live ? std::max(13, int(m_px * 0.85)) : std::max(13, m_px - 2));

    QTextCharFormat faint;
    faint.setForeground(k.faint);

    // Fenced code.
    if (md::isFenceLine(text)) {
        setCurrentBlockState(prev & 1 ? 0 : 1);
        info->kind = BlockInfo::Fence;
        QTextCharFormat f = fontFormat(mono);
        f.setForeground(k.faint);
        setFormat(0, int(text.size()), f);
        if (!active) hide(0, int(text.size()));
        return;
    }
    if (prev & 1) {
        setCurrentBlockState(1);
        info->kind = BlockInfo::Code;
        QTextCharFormat f = fontFormat(mono);
        f.setForeground(k.ink2);
        setFormat(0, int(text.size()), f);
        return;
    }
    setCurrentBlockState(0);

    QTextCharFormat base;
    int from = 0;

    // Headings.
    if (auto m = re(R"(^(#{1,6})(\s+))").match(text); m.hasMatch()) {
        info->kind = BlockInfo::Heading;
        info->level = int(m.capturedLength(1));
        static const qreal scale[] = {1.6, 1.3, 1.1, 1.0, 1.0, 1.0};
        QFont f = live ? Theme::serif(m_px * scale[info->level - 1], QFont::DemiBold) : Theme::mono(std::max(13, m_px - 2));
        if (!live) f.setBold(true);
        base = fontFormat(f);
        base.setForeground(k.ink);
        setFormat(0, int(text.size()), base);
        from = int(m.capturedEnd());
        if (!active) hide(0, from);
        else {
            QTextCharFormat mk = base;
            mk.setForeground(k.faint);
            setFormat(0, from, mk);
        }
        inlineSpans(text, from, info, live, base);
        return;
    }
    if (!live) {
        base = fontFormat(mono);
        setFormat(0, int(text.size()), base);
    }

    // Thematic break.
    if (re(R"(^\s{0,3}([-*_])(\s*\1){2,}\s*$)").match(text).hasMatch()) {
        info->kind = BlockInfo::Rule;
        QTextCharFormat f = base;
        f.setForeground(active ? k.faint : QColor(Qt::transparent));
        setFormat(0, int(text.size()), f);
        return;
    }

    // An image alone on its line becomes the image.
    if (auto m = re(R"(^\s*!\[([^\]\n]*)\]\(ivy://res/([A-Za-z0-9_-]{8,64})\)\s*$)").match(text); m.hasMatch()) {
        const QString id = m.captured(2);
        const QString kind = attachmentKind ? attachmentKind(id) : QString();
        if (kind.isEmpty() || kind == QLatin1String("image")) {
            info->kind = BlockInfo::Image;
            info->imageId = id;
            if (live && !active) {
                const QSize size = displaySize(id);
                info->imageSize = size;
                hide(0, int(text.size()));
                // One huge, invisible, zero-width character makes the line as
                // tall as the image, so layout, scrolling and hit-testing all
                // stay native.
                QFont probe = Theme::uiFont(100);
                const qreal ratio = QFontMetricsF(probe).height() / 100.0;
                const qreal lineFactor = 1.75;
                const int px = std::max(8, int((size.height() + 14) / (ratio * lineFactor)));
                QTextCharFormat tall;
                tall.setForeground(Qt::transparent);
                tall.setProperty(QTextFormat::FontPixelSize, px);
                tall.setFontLetterSpacingType(QFont::PercentageSpacing);
                tall.setFontLetterSpacing(1);
                setFormat(0, 1, tall);
                return;
            }
        }
    }

    // Block quote.
    if (auto m = re(R"(^(\s{0,3}>\s?))").match(text); m.hasMatch()) {
        info->kind = BlockInfo::Quote;
        if (live) {
            base = fontFormat(Theme::serif(m_px, QFont::Normal));
            base.setForeground(k.ink2);
            setFormat(0, int(text.size()), base);
        }
        from = int(m.capturedEnd());
        if (live && !active) {
            // The marker stays as invisible indent; the bar is drawn in it.
            QTextCharFormat indent = base;
            indent.setForeground(Qt::transparent);
            indent.setFontLetterSpacingType(QFont::AbsoluteSpacing);
            indent.setFontLetterSpacing(4);
            setFormat(0, from, indent);
        } else {
            setFormat(0, from, faint);
        }
    }

    // Lists and tasks.
    if (auto m = re(R"(^(\s*)([-*+]|\d+[.)])(\s+)(\[[ xX]\]\s+)?)").match(text, from); m.hasMatch() && m.capturedStart() == from) {
        const bool ordered = m.captured(2).front().isDigit();
        const int markStart = int(m.capturedStart(2));
        const int markLen = int(m.capturedLength(2));
        if (!m.captured(4).isEmpty()) {
            const int box = int(m.capturedStart(4));
            const bool done = m.captured(4).at(1) != u' ';
            if (live && !active) {
                // The marker vanishes; the box keeps its width under a drawn checkbox.
                if (!ordered) hide(markStart, markLen + int(m.capturedLength(3)));
                // Widen the hidden "[ ]" so the drawn box has room.
                QTextCharFormat clear = base;
                clear.setForeground(Qt::transparent);
                clear.setFontLetterSpacingType(QFont::AbsoluteSpacing);
                clear.setFontLetterSpacing(std::max(2.0, m_px * 0.2));
                setFormat(box, 3, clear);
                info->tasks.append({box, done});
                if (done) {
                    QTextCharFormat d = base;
                    d.setForeground(k.muted);
                    d.setFontStrikeOut(true);
                    setFormat(int(m.capturedEnd()), int(text.size() - m.capturedEnd()), d);
                }
            } else {
                setFormat(markStart, markLen, faint);
                setFormat(box, 3, faint);
            }
        } else if (!ordered) {
            if (live && !active) {
                QTextCharFormat clear = base;
                clear.setForeground(Qt::transparent);
                setFormat(markStart, markLen, clear);
                info->bullets << markStart;
            } else {
                setFormat(markStart, markLen, faint);
            }
        } else {
            QTextCharFormat num = base;
            num.setForeground(k.muted);
            setFormat(markStart, markLen, num);
        }
        from = int(m.capturedEnd());
    }

    inlineSpans(text, from, info, live, base);
}

void MarkdownHighlighter::inlineSpans(const QString &text, int from, BlockInfo *info, bool live, const QTextCharFormat &base) {
    const Tokens &k = Theme::t();
    const bool hideMarks = live && !info->active;
    QTextCharFormat faint = base;
    faint.setForeground(k.faint);
    auto marker = [&](int s, int l) {
        if (hideMarks) hide(s, l);
        else setFormat(s, l, faint);
    };
    // Ranges already claimed (code spans, links) are skipped by later rules.
    QList<QPair<int, int>> taken;
    auto free = [&](int s, int e) {
        for (const auto &[a, b] : taken)
            if (s < b && e > a) return false;
        return true;
    };
    auto merge = [&](int s, int l, const std::function<void(QTextCharFormat &)> &f) {
        for (int i = s; i < s + l; ++i) {
            QTextCharFormat cf = format(i);
            f(cf);
            setFormat(i, 1, cf);
        }
    };

    // Inline code.
    for (auto it = re(R"(`([^`\n]+)`)").globalMatch(text, from); it.hasNext();) {
        const auto m = it.next();
        const int s = int(m.capturedStart()), e = int(m.capturedEnd());
        taken.append({s, e});
        QTextCharFormat c = fontFormat(Theme::mono(std::max(12, int(m_px * 0.85))));
        c.setForeground(k.ink);
        c.setBackground(k.codeBg.darker(Theme::dark() ? 80 : 104));
        setFormat(s + 1, e - s - 2, c);
        marker(s, 1);
        marker(e - 1, 1);
    }
    // Wiki links.
    for (auto it = re(R"(\[\[([^\[\]\n]+?)\]\])").globalMatch(text, from); it.hasNext();) {
        const auto m = it.next();
        const int s = int(m.capturedStart()), e = int(m.capturedEnd());
        if (!free(s, e)) continue;
        taken.append({s, e});
        const QString inner = m.captured(1);
        const md::WikiLink w = md::parseWiki(inner);
        const bool exists = !wikiExists || wikiExists(w.target);
        info->links.append({s, e, inner, true});
        const int bar = int(inner.indexOf(u'|'));
        merge(s + 2, e - s - 4, [&](QTextCharFormat &f) {
            f.setForeground(exists ? k.ivy : k.muted);
            if (!exists) {
                f.setUnderlineStyle(QTextCharFormat::DashUnderline);
                f.setUnderlineColor(k.faint);
            }
        });
        marker(s, 2);
        marker(e - 2, 2);
        if (hideMarks && bar >= 0) hide(s + 2, bar + 1);
    }
    // Links [text](url); images inline in text stay as source.
    for (auto it = re(R"((!?)\[([^\]\n]+)\]\(([^)\s]+)(\s+"[^"\n]*")?\))").globalMatch(text, from); it.hasNext();) {
        const auto m = it.next();
        const int s = int(m.capturedStart()), e = int(m.capturedEnd());
        if (!free(s, e)) continue;
        taken.append({s, e});
        if (!m.captured(1).isEmpty()) {
            merge(s, e - s, [&](QTextCharFormat &f) { f.setForeground(k.muted); });
            info->links.append({s, e, m.captured(3), false});
            continue;
        }
        info->links.append({s, e, m.captured(3), false});
        const int textStart = int(m.capturedStart(2)), textEnd = int(m.capturedEnd(2));
        merge(textStart, textEnd - textStart, [&](QTextCharFormat &f) {
            f.setForeground(k.ivy);
            f.setFontUnderline(true);
            f.setUnderlineColor(Theme::alpha(k.ivy, 0.45));
        });
        marker(s, 1);
        marker(textEnd, e - textEnd);
    }
    // Bare URLs.
    for (auto it = re(R"(https?://[^\s<>()\[\]]+[^\s<>()\[\].,;:!?'"])").globalMatch(text, from); it.hasNext();) {
        const auto m = it.next();
        const int s = int(m.capturedStart()), e = int(m.capturedEnd());
        if (!free(s, e)) continue;
        taken.append({s, e});
        info->links.append({s, e, m.captured(0), false});
        merge(s, e - s, [&](QTextCharFormat &f) {
            f.setForeground(k.ivy);
            f.setFontUnderline(true);
            f.setUnderlineColor(Theme::alpha(k.ivy, 0.45));
        });
    }
    // Emphasis.
    struct Rule {
        const char *pattern;
        int mark;
        std::function<void(QTextCharFormat &)> apply;
    };
    const QList<Rule> rules{
        {R"(\*\*(?=\S)(.+?)(?<=\S)\*\*)", 2, [](QTextCharFormat &f) { f.setFontWeight(QFont::Bold); }},
        {R"((?<![\w_])__(?=\S)(.+?)(?<=\S)__(?![\w_]))", 2, [](QTextCharFormat &f) { f.setFontWeight(QFont::Bold); }},
        {R"((?<![*\w])\*(?=[^\s*])(.+?)(?<=[^\s*])\*(?![*\w]))", 1, [](QTextCharFormat &f) { f.setFontItalic(true); }},
        {R"((?<![_\w])_(?=[^\s_])(.+?)(?<=[^\s_])_(?![_\w]))", 1, [](QTextCharFormat &f) { f.setFontItalic(true); }},
        {R"(~~(?=\S)(.+?)(?<=\S)~~)", 2, [&k](QTextCharFormat &f) { f.setFontStrikeOut(true); f.setForeground(k.muted); }},
        {R"(==(?=\S)(.+?)(?<=\S)==)", 2, [&k](QTextCharFormat &f) { f.setBackground(Theme::alpha(k.warn, 0.25)); }},
    };
    for (const Rule &rule : rules) {
        for (auto it = re(rule.pattern).globalMatch(text, from); it.hasNext();) {
            const auto m = it.next();
            const int s = int(m.capturedStart()), e = int(m.capturedEnd());
            if (!free(s, e)) continue;
            merge(s + rule.mark, e - s - 2 * rule.mark, rule.apply);
            marker(s, rule.mark);
            marker(e - rule.mark, rule.mark);
        }
    }
}

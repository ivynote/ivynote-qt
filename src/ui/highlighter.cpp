#include "ui/highlighter.h"

#include "core/markdown.h"
#include "core/tasks.h"
#include "ui/remoteimages.h"
#include "ui/theme.h"
#include "ui/render.h"
#include "core/i18n.h"
#include <QPainter>

#include <QDate>
#include <QFontMetricsF>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextDocument>
#include <cmath>
#include <QUrl>

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

namespace {
// A read-only layout painted over an inactive source block. QTextDocument
// handles inline image positioning/wrapping; the actual editor stays plain text.
class ImageParagraphDocument : public QTextDocument {
public:
    std::function<QImage(const QString &)> loader;
    int imageWidth = 320;
protected:
    QVariant loadResource(int type, const QUrl &url) override {
        if (type != QTextDocument::ImageResource || !loader) return {};
        QString key;
        if (RemoteImages::isRemote(url.toString())) key = url.toString();
        else if (url.scheme() == QLatin1String("ivy") && url.host() == QLatin1String("res")) key = url.path().mid(1);
        if (key.isEmpty()) return {};
        QImage image = loader(key);
        if (image.isNull()) {
            image = QImage(std::min(320, imageWidth), 120, QImage::Format_ARGB32_Premultiplied);
            image.fill(Theme::t().sunken);
            QPainter painter(&image);
            painter.setPen(Theme::t().muted);
            painter.setFont(Theme::uiFont(12));
            painter.drawText(image.rect().adjusted(6, 6, -6, -6), Qt::AlignCenter | Qt::TextWordWrap,
                             RemoteImages::isRemote(key) && RemoteImages::instance()->failed(key) ? T("图片加载失败，右键可重试") : T("图片加载中…"));
        }
        if (image.width() > imageWidth) image = image.scaledToWidth(imageWidth, Qt::SmoothTransformation);
        return image;
    }
};
} // namespace

qreal MarkdownHighlighter::advance(const QTextCharFormat &f, const QString &text) const {
    if (text.isEmpty()) return 0;
    QFont font = f.font().resolve(document()->defaultFont());
    if (!f.hasProperty(QTextFormat::FontFamilies) && !f.hasProperty(QTextFormat::FontPixelSize) && !f.hasProperty(QTextFormat::FontPointSize))
        font = document()->defaultFont();
    if (f.hasProperty(QTextFormat::FontLetterSpacing)) font.setLetterSpacing(QFont::SpacingType(f.fontLetterSpacingType()), f.fontLetterSpacing());
    return QFontMetricsF(font).horizontalAdvance(text);
}

QTextCharFormat MarkdownHighlighter::tallChar(qreal height, qreal lineFactor) const {
    // One invisible, nearly zero-width character sized so its line is
    // `height` tall: images, table rows and code padding use it.
    const qreal ratio = QFontMetricsF(Theme::uiFont(100)).height() / 100.0;
    QTextCharFormat tall;
    tall.setForeground(Qt::transparent);
    tall.setProperty(QTextFormat::FontPixelSize, std::max(1, int(std::round(height / (ratio * lineFactor)))));
    tall.setFontLetterSpacingType(QFont::PercentageSpacing);
    tall.setFontLetterSpacing(1);
    return tall;
}

MarkdownHighlighter::MarkdownHighlighter(QTextDocument *doc) : QSyntaxHighlighter(doc) {
    connect(Theme::instance(), &Theme::changed, this, &QSyntaxHighlighter::rehighlight);
    m_referenceTimer.setSingleShot(true);
    m_referenceTimer.setInterval(120);
    connect(doc, &QTextDocument::contentsChange, this, [this](int, int removed, int added) {
        if (removed || added) m_referenceTimer.start();
    });
    connect(&m_referenceTimer, &QTimer::timeout, this, [this] {
        const QString definitions = md::imageDefinitions(document()->toPlainText());
        if (definitions == m_referenceDefinitions) return;
        m_referenceDefinitions = definitions;
        // A definition can change images in completely different blocks.
        rehighlight();
    });
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
    // Note what the line was, to tell the editor when its layout must change.
    struct Watch {
        MarkdownHighlighter *h;
        int num;
        int kind;
        qreal hang;
        BlockInfo *info = nullptr;
        ~Watch() {
            if (info && (int(info->kind) != kind || info->hang != hang)) h->m_changed.insert(num);
        }
    };
    const auto *old = static_cast<BlockInfo *>(currentBlockUserData());
    Watch watch{this, num, old ? int(old->kind) : int(BlockInfo::Text), old ? old->hang : 0.0};
    auto *info = new BlockInfo;
    info->active = active;
    setCurrentBlockUserData(info);
    watch.info = info;
    const QFont mono = Theme::mono(live ? std::max(13, int(m_px * 0.85)) : std::max(13, m_px - 2));

    QTextCharFormat faint;
    faint.setForeground(k.faint);

    // Fenced code.
    int fence = prev >= 6 ? prev : 0;
    if (md::advanceFence(text, fence)) {
        setCurrentBlockState(fence);
        info->kind = BlockInfo::Fence;
        QTextCharFormat f = fontFormat(mono);
        f.setForeground(k.faint);
        setFormat(0, int(text.size()), f);
        if (!active && live) {
            // The fence vanishes but keeps a little height: the panel's padding.
            hide(0, int(text.size()));
            if (!text.isEmpty()) setFormat(0, 1, tallChar(m_px * 0.55, 1.5));
        }
        return;
    }
    if (fence) {
        setCurrentBlockState(fence);
        info->kind = BlockInfo::Code;
        QTextCharFormat f = fontFormat(mono);
        f.setForeground(k.ink2);
        setFormat(0, int(text.size()), f);
        return;
    }
    setCurrentBlockState(0);
    if (tableBlock(text, info, live, active, mono)) return;

    QTextCharFormat base;
    int from = 0;

    // Reference targets are metadata, not body text. Reveal them on edit.
    if (re(R"(^ {0,3}\[[^\]\n]+\]:\s*\S)").match(text).hasMatch()) {
        info->kind = BlockInfo::ReferenceDefinition;
        if (live && !active) hide(0, int(text.size()));
        else { QTextCharFormat f = fontFormat(mono); f.setForeground(k.muted); setFormat(0, int(text.size()), f); }
        return;
    }

    const QString definitions = text.contains(QLatin1String("![")) && !text.contains(QLatin1String("](")) ? md::imageDefinitions(document()->toPlainText()) : QString();
    // Resolve complete image syntax through md4c: empty alt, angle URLs,
    // balanced parentheses, titles, and self-labelled legacy image links.
    if (const md::ImageLink img = md::standaloneImage(text, definitions); !img.url.isEmpty()) {
        const bool remote = RemoteImages::isRemote(img.url);
        const QUrl url(img.url);
        const QString id = remote ? img.url : url.scheme() == QLatin1String("ivy") && url.host() == QLatin1String("res") ? url.path().mid(1) : QString();
        const QString kind = !remote && attachmentKind ? attachmentKind(id) : QString();
        if (!id.isEmpty() && (remote || kind.isEmpty() || kind == QLatin1String("image"))) {
            info->kind = BlockInfo::Image;
            info->imageId = id;
            if (live && !active) {
                info->imageSize = displaySize(id);
                hide(0, int(text.size()));
                if (!text.isEmpty()) setFormat(0, 1, tallChar(info->imageSize.height() + 14));
                return;
            }
        }
    }

    // Images embedded in a paragraph or wrapped in a link must be drawn as
    // images too, rather than leaving their Markdown URL visible.
    if (live && !active && text.contains(QLatin1String("![")) && info->kind != BlockInfo::Image) {
        RenderOptions opts;
        opts.remoteImages = true;
        opts.resourceKind = attachmentKind;
        const QString html = renderMarkdown(text + QStringLiteral("\n\n") + definitions, opts);
        if (html.contains(QLatin1String("<img "))) {
            auto doc = QSharedPointer<ImageParagraphDocument>::create();
            doc->loader = imageLoader;
            doc->imageWidth = std::max(1, m_columnWidth - 8);
            doc->setDocumentMargin(0);
            doc->setDefaultFont(Theme::uiFont(m_px));
            doc->setDefaultStyleSheet(QStringLiteral("p{margin:0;color:%1;}a{color:%2;}code{background:%3;}").arg(k.ink.name(), k.ivy.name(), k.codeBg.name()));
            // Give paragraph images their own visual line; a full-width
            // photo must not leave one orphaned character beside it.
            QString flow = html;
            static const QRegularExpression imageTag(QStringLiteral(R"((<img [^>]*>))"));
            flow.replace(imageTag, QStringLiteral("<br>\\1<br>"));
            doc->setHtml(flow);
            doc->setTextWidth(doc->imageWidth);
            info->kind = BlockInfo::ImageParagraph;
            info->imageDocument = doc;
            info->imageSize = doc->size().toSize();
            hide(0, int(text.size()));
            if (!text.isEmpty()) setFormat(0, 1, tallChar(info->imageSize.height() + 14));
            return;
        }
    }

    // Headings.
    if (auto m = re(R"(^(#{1,6})(\s+))").match(text); m.hasMatch()) {
        if (info->kind != BlockInfo::Image) info->kind = BlockInfo::Heading;
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

    // Block quote.
    if (auto m = re(R"(^(\s{0,3}>\s?))").match(text); m.hasMatch()) {
        info->kind = BlockInfo::Quote;
        if (live) {
            base = fontFormat(Theme::serif(m_px, QFont::Normal));
            base.setForeground(k.ink2);
            setFormat(0, int(text.size()), base);
        }
        from = int(m.capturedEnd());
        // The marker keeps the same width shown or hidden (the bar is drawn
        // in it), so revealing the source never shifts the text; wrapped
        // lines hang under the text, not under the bar.
        QTextCharFormat mark = live ? base : fontFormat(Theme::mono(std::max(13, m_px - 2)));
        mark.setForeground(live && !active ? QColor(Qt::transparent) : k.faint);
        if (live) {
            mark.setFontLetterSpacingType(QFont::AbsoluteSpacing);
            mark.setFontLetterSpacing(6);
        }
        setFormat(0, from, mark);
        info->hang = advance(mark, text.left(from));
    }

    // Lists and tasks.
    if (auto m = re(R"(^(\s*)([-*+]|\d+[.)])(\s+)(\[[ xX]\]\s+)?)").match(text, from); m.hasMatch() && m.capturedStart() == from) {
        const bool ordered = m.captured(2).front().isDigit();
        QTextCharFormat faint = base; // markers keep the line's font, so widths match
        faint.setForeground(k.faint);
        const int markStart = int(m.capturedStart(2));
        const int markLen = int(m.capturedLength(2));
        if (!m.captured(4).isEmpty()) {
            const int box = int(m.capturedStart(4));
            const bool done = m.captured(4).at(1) != u' ';
            if (live && !active) {
                // "- [ ]" turns invisible at its own width; the checkbox is
                // drawn over it (where the bullet was, for "- [ ]").
                QTextCharFormat clear = base;
                clear.setForeground(Qt::transparent);
                setFormat(markStart, markLen, clear);
                setFormat(box, 3, clear);
                info->tasks.append({box, done});
                info->taskBoxes.append(ordered ? box : markStart);
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
            // A little air after the bullet, the same shown or hidden.
            QTextCharFormat mark = live && !active ? base : faint;
            if (live && !active) mark.setForeground(Qt::transparent);
            if (live) {
                mark.setFontLetterSpacingType(QFont::AbsoluteSpacing);
                mark.setFontLetterSpacing(6);
            }
            setFormat(markStart, markLen, mark);
            if (live && !active) info->bullets << markStart;
        } else {
            QTextCharFormat num = base;
            num.setForeground(k.muted);
            setFormat(markStart, markLen, num);
        }
        info->hang += advance(base, text.mid(from, m.capturedEnd() - from));
        from = int(m.capturedEnd());
    }

    inlineSpans(text, from, info, live, base);
}

bool MarkdownHighlighter::tableBlock(const QString &text, BlockInfo *info, bool live, bool active, const QFont &mono) {
    const Tokens &k = Theme::t();
    const QTextBlock cur = currentBlock();
    const QTextBlock before = cur.previous();
    const auto *prevInfo = before.isValid() ? static_cast<BlockInfo *>(before.userData()) : nullptr;
    const bool prevInTable = previousBlockState() == 2 && prevInfo && prevInfo->kind == BlockInfo::Table;
    // The header is known only by the delimiter under it, so a change to the
    // delimiter line re-reads the line above.
    auto recheckAbove = [this, n = cur.blockNumber() - 1] {
        QMetaObject::invokeMethod(this, [this, n] {
            const QTextBlock b = document()->findBlockByNumber(n);
            if (b.isValid()) rehighlightBlock(b);
        }, Qt::QueuedConnection);
    };
    const bool delimiter = md::isTableDelimiter(text);
    const bool prevIsHeader = prevInTable && prevInfo->tableRow == 0;
    if (prevIsHeader && !delimiter) {
        recheckAbove();
        return false;
    }
    if (delimiter && !prevIsHeader && before.isValid() && md::isTableRow(before.text()) && !md::isTableDelimiter(before.text()) && !prevInTable) recheckAbove();
    const bool row = prevInTable && md::isTableRow(text);
    const QTextBlock after = cur.next();
    const bool header = !row && md::isTableRow(text) && !delimiter && after.isValid() && md::isTableDelimiter(after.text());
    if (!row && !header) return false;

    setCurrentBlockState(2);
    info->kind = BlockInfo::Table;
    info->tableRow = header ? 0 : prevInfo->tableRow + 1;
    if (live && !active) {
        // The source vanishes and the editor paints the grid over it. One
        // invisible character sets the row height (as images do); the
        // delimiter row shrinks to a hairline under the header.
        hide(0, int(text.size()));
        if (info->tableRow != 1 && !text.isEmpty()) {
            // Tall enough for the row's most wrapped cell.
            const TableGeometry g = tableGeometry(cur);
            const int i = cur.blockNumber() - g.start;
            setFormat(0, 1, tallChar(i >= 0 && i < g.rowHeight.size() ? g.rowHeight[i] : m_px * 2.1));
        }
        return true;
    }
    // Source: monospace so the columns line up, pipes and dashes quiet.
    QTextCharFormat base = fontFormat(live ? mono : Theme::mono(std::max(13, m_px - 2)));
    base.setForeground(k.ink2);
    if (info->tableRow == 0) base.setFontWeight(QFont::DemiBold);
    setFormat(0, int(text.size()), base);
    QTextCharFormat faint = base;
    faint.setForeground(k.faint);
    faint.setFontWeight(QFont::Normal);
    if (delimiter) {
        setFormat(0, int(text.size()), faint);
        return true;
    }
    inlineSpans(text, 0, info, live, base);
    for (int i = 0; i < text.size(); ++i)
        if (text[i] == u'|' && (i == 0 || text[i - 1] != u'\\')) setFormat(i, 1, faint);
    return true;
}

MarkdownHighlighter::TableGeometry MarkdownHighlighter::tableGeometry(const QTextBlock &row) const {
    auto isHeader = [](const QTextBlock &b) {
        return md::isTableRow(b.text()) && !md::isTableDelimiter(b.text()) && b.next().isValid() && md::isTableDelimiter(b.next().text());
    };
    QTextBlock start = row;
    while (!isHeader(start) && start.previous().isValid() && md::isTableRow(start.previous().text())) start = start.previous();
    if (!isHeader(start)) return {};
    // One pass per table and edit, shared by every row.
    const int rev = document()->revision();
    if (m_tableRevision != rev || m_tablePx != m_px || m_tableWidth != m_columnWidth) {
        m_tables.clear();
        m_tableRevision = rev;
        m_tablePx = m_px;
        m_tableWidth = m_columnWidth;
    }
    if (auto it = m_tables.constFind(start.blockNumber()); it != m_tables.constEnd()) return *it;

    TableGeometry g;
    g.start = start.blockNumber();
    QList<QStringList> rows;
    for (QTextBlock b = start; b.isValid() && md::isTableRow(b.text()); b = b.next()) {
        if (b != start && rows.size() != 1 && isHeader(b)) break;
        rows << md::tableCells(b.text());
    }
    g.cols = int(std::max<qsizetype>(1, rows.value(0).size()));
    g.align = md::tableAlignments(start.next().text());
    while (g.align.size() < g.cols) g.align << int(Qt::AlignLeft);
    for (QStringList &r : rows)
        for (QString &c : r) c = md::plainInline(c);
    const QFontMetricsF head(Theme::uiFont(m_px - 2, QFont::Medium));
    const QFontMetricsF body(Theme::uiFont(m_px - 2));
    const qreal pad = 12;
    // Widths: natural when they fit; otherwise narrow columns keep theirs
    // and the wide ones share what is left, in proportion.
    QList<qreal> natural(g.cols, 48);
    for (int i = 0; i < rows.size(); ++i) {
        if (i == 1) continue;
        for (int c = 0; c < g.cols && c < rows[i].size(); ++c)
            natural[c] = std::max(natural[c], (i == 0 ? head : body).horizontalAdvance(rows[i][c]) + 2 * pad + 1);
    }
    const qreal avail = std::max(120, m_columnWidth - 2);
    QList<qreal> w = natural;
    qreal sum = 0;
    for (qreal x : natural) sum += x;
    if (sum > avail) {
        QList<bool> fixed(g.cols, false);
        qreal room = avail;
        for (bool changed = true; changed;) {
            changed = false;
            int open = 0;
            for (int c = 0; c < g.cols; ++c) open += !fixed[c];
            if (!open) break;
            for (int c = 0; c < g.cols; ++c)
                if (!fixed[c] && natural[c] <= room / open) {
                    fixed[c] = true;
                    room -= natural[c];
                    changed = true;
                }
        }
        qreal rest = 0;
        for (int c = 0; c < g.cols; ++c)
            if (!fixed[c]) rest += natural[c];
        for (int c = 0; c < g.cols; ++c)
            if (!fixed[c]) w[c] = std::max<qreal>(56, room * natural[c] / std::max<qreal>(1, rest));
    }
    g.x << 0;
    for (qreal x : w) g.x << g.x.last() + std::round(x);
    // Row heights: the tallest wrapped cell plus padding.
    for (int i = 0; i < rows.size(); ++i) {
        if (i == 1) {
            g.rowHeight << 2;
            continue;
        }
        qreal h = (i == 0 ? head : body).height();
        for (int c = 0; c < g.cols && c < rows[i].size(); ++c) {
            const QRectF r = (i == 0 ? head : body).boundingRect(QRectF(0, 0, w[c] - 2 * pad, 1e6), Qt::TextWordWrap, rows[i][c]);
            h = std::max(h, r.height());
        }
        g.rowHeight << std::ceil(h + 16);
    }
    m_tables.insert(g.start, g);
    return g;
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
    for (auto it = re(R"((?<!`)(`+)(?!`)(.+?)(?<!`)\1(?!`))").globalMatch(text, from); it.hasNext();) {
        const auto m = it.next();
        const int s = int(m.capturedStart()), e = int(m.capturedEnd());
        if (!free(s, e)) continue;
        taken.append({s, e});
        const int ticks = int(m.capturedLength(1));
        QTextCharFormat c = fontFormat(Theme::mono(std::max(12, int(m_px * 0.85))));
        c.setForeground(k.ink);
        c.setBackground(k.codeBg.darker(Theme::dark() ? 80 : 104));
        setFormat(s + ticks, e - s - 2 * ticks, c);
        marker(s, ticks);
        marker(e - ticks, ticks);
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
    // Task metadata markers (📅 due, 🔁 repeat, ✅ done) on task lines.
    if (tasks::isTaskLine(text)) {
        const auto parsed = tasks::parseTaskLine(text);
        const bool taskDone = parsed && parsed->done;
        const QDate today = QDate::currentDate();
        for (auto it = re(R"(📅️?\s*(\d{4}-\d{2}-\d{2})(?:[ T](\d{1,2}:\d{2}))?)").globalMatch(text, from); it.hasNext();) {
            const auto m = it.next();
            const int s = int(m.capturedStart()), e = int(m.capturedEnd());
            if (!free(s, e)) continue;
            taken.append({s, e});
            info->taskMarkers.append({s, e, TaskMarkerSpan::Due});
            const QDate d = QDate::fromString(m.captured(1), Qt::ISODate);
            QColor fg = k.muted;
            QColor bg = Theme::alpha(k.line, 0.55);
            if (!taskDone && d.isValid()) {
                if (d < today) {
                    fg = k.danger;
                    bg = Theme::alpha(k.danger, 0.12);
                } else if (d == today) {
                    fg = k.ivy;
                    bg = Theme::alpha(k.ivy, 0.14);
                }
            }
            merge(s, e - s, [&](QTextCharFormat &f) {
                f.setForeground(fg);
                f.setBackground(bg);
                f.setFontStrikeOut(false);
                if (live) f.setProperty(QTextFormat::FontPixelSize, std::max(12, m_px - 2));
            });
        }
        for (auto it = re(R"(🔁️?\s*([^📅✅⏳🛫➕⏫🔼🔽🔺⏬\n]+?)(?=\s*(?:📅|✅|⏳|🛫|➕|⏫|🔼|🔽|🔺|⏬|$)))").globalMatch(text, from); it.hasNext();) {
            const auto m = it.next();
            const int s = int(m.capturedStart()), e = int(m.capturedEnd());
            if (!free(s, e)) continue;
            taken.append({s, e});
            info->taskMarkers.append({s, e, TaskMarkerSpan::Repeat});
            merge(s, e - s, [&](QTextCharFormat &f) {
                f.setForeground(taskDone ? k.muted : k.ivy);
                f.setBackground(Theme::alpha(taskDone ? k.line : k.ivy, 0.10));
                f.setFontStrikeOut(false);
                if (live) f.setProperty(QTextFormat::FontPixelSize, std::max(12, m_px - 2));
            });
        }
        for (auto it = re(R"(✅️?\s*(\d{4}-\d{2}-\d{2}))").globalMatch(text, from); it.hasNext();) {
            const auto m = it.next();
            const int s = int(m.capturedStart()), e = int(m.capturedEnd());
            if (!free(s, e)) continue;
            taken.append({s, e});
            info->taskMarkers.append({s, e, TaskMarkerSpan::Done});
            merge(s, e - s, [&](QTextCharFormat &f) {
                f.setForeground(k.faint);
                f.setFontStrikeOut(false);
                if (live) f.setProperty(QTextFormat::FontPixelSize, std::max(12, m_px - 2));
            });
        }
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

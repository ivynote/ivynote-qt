#include "ui/editor.h"

#include "core/i18n.h"
#include "core/markdown.h"
#include "ui/theme.h"

#include <QAbstractItemView>
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QCompleter>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStringListModel>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>

MarkdownEditor::MarkdownEditor(QWidget *parent)
    : QTextEdit(parent), m_hl(new MarkdownHighlighter(document())), m_completer(new QCompleter(this)),
      m_titles(new QStringListModel(this)) {
    setAcceptRichText(false);
    setFrameShape(QFrame::NoFrame);
    setTabChangesFocus(false);
    setLineWrapMode(QTextEdit::WidgetWidth);
    setAutoFillBackground(true);
    viewport()->setCursor(Qt::IBeamCursor);
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    document()->setDocumentMargin(2);
    setCursorWidth(2);
    setPlaceholderText(T("开始书写… 输入 [[ 链接其他笔记"));

    m_hl->wikiExists = [this](const QString &t) { return !wikiExists || wikiExists(t); };
    m_hl->imageSize = [this](const QString &id) -> QSize {
        if (image) {
            const QImage img = image(id);
            if (!img.isNull()) return img.size() / std::max(1.0, img.devicePixelRatio());
        }
        return imageMeta ? imageMeta(id) : QSize();
    };
    m_hl->attachmentKind = [this](const QString &id) { return attachmentKind ? attachmentKind(id) : QString(); };

    m_completer->setModel(m_titles);
    m_completer->setWidget(this);
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_completer->setFilterMode(Qt::MatchContains);
    m_completer->setMaxVisibleItems(8);
    connect(m_completer, qOverload<const QString &>(&QCompleter::activated), this, &MarkdownEditor::insertCompletion);

    connect(this, &QTextEdit::cursorPositionChanged, this, [this] {
        updateActive();
        const int line = textCursor().blockNumber();
        if (line != m_lastLine) {
            m_lastLine = line;
            emit cursorLineChanged(line);
        }
    });
    connect(this, &QTextEdit::selectionChanged, this, &MarkdownEditor::updateActive);
    connect(document(), &QTextDocument::contentsChange, this, [this](int, int removed, int added) {
        if (m_loading || (removed == 0 && added == 0)) return;
        emit edited();
    });
    connect(Theme::instance(), &Theme::changed, this, [this] {
        QPalette p = palette();
        p.setColor(QPalette::Base, Theme::t().surface);
        p.setColor(QPalette::Window, Theme::t().surface);
        p.setColor(QPalette::Text, Theme::t().ink);
        setPalette(p);
        viewport()->update();
    });
    QPalette p = palette();
    p.setColor(QPalette::Base, Theme::t().surface);
    p.setColor(QPalette::Window, Theme::t().surface);
    setPalette(p);
    setFontSize(16);
}

void MarkdownEditor::setMarkdown(const QString &text) {
    m_loading = true;
    setUndoRedoEnabled(false);
    setPlainText(text);
    applyLineHeight();
    setUndoRedoEnabled(true);
    m_loading = false;
    QTextCursor c(document());
    c.movePosition(QTextCursor::Start);
    setTextCursor(c);
    m_lastLine = -1;
    updateActive();
}

void MarkdownEditor::applyLineHeight() {
    // Block formats only change while loading, so they never reach the undo
    // stack; new lines inherit them.
    QTextCursor c(document());
    c.select(QTextCursor::Document);
    QTextBlockFormat bf;
    bf.setLineHeight(m_hl->mode() == MarkdownHighlighter::Live ? 175 : 165, QTextBlockFormat::ProportionalHeight);
    c.mergeBlockFormat(bf);
    // Room to scroll the last line up from the bottom edge.
    QTextFrameFormat ff = document()->rootFrame()->frameFormat();
    ff.setBottomMargin(std::max(120, int(viewport()->height() * 0.4)));
    ff.setTopMargin(6);
    document()->rootFrame()->setFrameFormat(ff);
}

void MarkdownEditor::applyRemote(const QString &text) {
    const QString cur = toPlainText();
    if (cur == text) return;
    qsizetype prefix = 0;
    const qsizetype max = std::min(cur.size(), text.size());
    while (prefix < max && cur[prefix] == text[prefix]) prefix++;
    qsizetype suffix = 0;
    while (suffix < max - prefix && cur[cur.size() - 1 - suffix] == text[text.size() - 1 - suffix]) suffix++;
    QTextCursor keep = textCursor();
    const int anchor = keep.anchor(), pos = keep.position();
    const int delta = int(text.size() - cur.size());
    const int changeEnd = int(cur.size() - suffix);
    m_loading = true;
    QTextCursor c(document());
    c.setPosition(int(prefix));
    c.setPosition(changeEnd, QTextCursor::KeepAnchor);
    c.beginEditBlock();
    c.insertText(text.mid(prefix, text.size() - suffix - prefix));
    c.endEditBlock();
    m_loading = false;
    auto shift = [&](int p) { return p >= changeEnd ? p + delta : std::min(p, int(prefix + (text.size() - suffix - prefix))); };
    QTextCursor n(document());
    n.setPosition(std::clamp(shift(anchor), 0, int(text.size())));
    n.setPosition(std::clamp(shift(pos), 0, int(text.size())), QTextCursor::KeepAnchor);
    setTextCursor(n);
}

void MarkdownEditor::setMode(MarkdownHighlighter::Mode m) {
    m_hl->setMode(m);
    setFontSize(m_px);
    m_loading = true;
    const bool undo = document()->isUndoRedoEnabled();
    // Line height changes should not land on the undo stack.
    document()->setUndoRedoEnabled(false);
    applyLineHeight();
    document()->setUndoRedoEnabled(undo);
    m_loading = false;
    updateActive();
    viewport()->update();
}

void MarkdownEditor::setFontSize(int px) {
    m_px = std::clamp(px, 13, 22);
    const bool live = m_hl->mode() == MarkdownHighlighter::Live;
    QFont f = live ? Theme::uiFont(m_px) : Theme::mono(std::max(13, m_px - 2));
    document()->setDefaultFont(f);
    setFont(f);
    m_hl->setFontSize(m_px);
}

void MarkdownEditor::setColumnWidth(int maxWidth) {
    m_maxColumn = maxWidth;
    updateMargins();
}

void MarkdownEditor::updateMargins() {
    const int w = width();
    const int side = std::max(24, (w - m_maxColumn) / 2);
    setViewportMargins(side, 0, side, 0);
    m_hl->setColumnWidth(std::min(m_maxColumn, w - 2 * side));
    m_hl->setViewportHeight(height());
    emit columnChanged();
}

QRect MarkdownEditor::textColumn() const {
    const int side = viewportMargins().left();
    // documentMargin matches the 2 px a frameless line edit keeps before its text.
    return {side, 0, std::max(0, width() - 2 * side), height()};
}

void MarkdownEditor::resizeEvent(QResizeEvent *e) {
    QTextEdit::resizeEvent(e);
    const int before = m_hl->property("cw").toInt();
    updateMargins();
    const int cw = std::min(m_maxColumn, width() - 2 * std::max(24, (width() - m_maxColumn) / 2));
    if (std::abs(cw - before) > 40) {
        // Image sizes depend on the column width.
        m_hl->setProperty("cw", cw);
        m_hl->rehighlight();
    }
}

void MarkdownEditor::setWikiTitles(const QStringList &titles) { m_titles->setStringList(titles); }

void MarkdownEditor::refreshAttachment(const QString &id) {
    for (QTextBlock b = document()->begin(); b.isValid(); b = b.next())
        if (b.text().contains(id)) m_hl->rehighlightBlock(b);
    viewport()->update();
}

void MarkdownEditor::updateActive() {
    // Only the line being edited shows its source.
    if (!hasFocus() && !m_completer->popup()->isVisible()) {
        m_hl->setActive(-1, -1);
        return;
    }
    const QTextCursor c = textCursor();
    QTextBlock first = document()->findBlock(c.selectionStart());
    QTextBlock last = document()->findBlock(c.selectionEnd());
    // A table shows its source as a whole, so its columns stay lined up.
    auto inTable = [](const QTextBlock &b) {
        const auto *info = b.isValid() ? static_cast<BlockInfo *>(b.userData()) : nullptr;
        return info && info->kind == BlockInfo::Table;
    };
    if (inTable(first))
        while (inTable(first.previous()) && static_cast<BlockInfo *>(first.userData())->tableRow > 0) first = first.previous();
    if (inTable(last))
        while (inTable(last.next()) && static_cast<BlockInfo *>(last.next().userData())->tableRow > 0) last = last.next();
    m_hl->setActive(first.blockNumber(), last.blockNumber());
}

void MarkdownEditor::scrollToLine(int line) {
    const QTextBlock b = document()->findBlockByNumber(line);
    if (!b.isValid()) return;
    QTextCursor c(b);
    setTextCursor(c);
    const QRectF r = document()->documentLayout()->blockBoundingRect(b);
    verticalScrollBar()->setValue(int(r.top()) - 24);
    setFocus();
}

QRectF MarkdownEditor::charRect(const QTextBlock &block, int pos) const {
    const QTextLayout *layout = block.layout();
    if (!layout) return {};
    const QRectF br = document()->documentLayout()->blockBoundingRect(block);
    const QTextLine line = layout->lineForTextPosition(pos);
    if (!line.isValid()) return {};
    const qreal x = line.cursorToX(pos);
    const qreal x2 = line.cursorToX(pos + 1);
    const QPointF off(-horizontalScrollBar()->value(), -verticalScrollBar()->value());
    return QRectF(br.left() + x, br.top() + line.y(), std::max(1.0, x2 - x), line.height()).translated(off);
}

void MarkdownEditor::paintEvent(QPaintEvent *e) {
    const Tokens &k = Theme::t();
    const bool live = m_hl->mode() == MarkdownHighlighter::Live;
    QAbstractTextDocumentLayout *layout = document()->documentLayout();
    const QPointF off(-horizontalScrollBar()->value(), -verticalScrollBar()->value());
    const QRectF visible = QRectF(viewport()->rect()).translated(-off);
    const qreal colLeft = document()->documentMargin() - 4;
    const qreal colRight = viewport()->width() - document()->documentMargin() + 4;

    // First and last visible blocks.
    QTextBlock first = cursorForPosition(QPoint(0, 0)).block();
    while (first.isValid() && first.previous().isValid() && layout->blockBoundingRect(first).top() > visible.top()) first = first.previous();

    {
        // Backgrounds under the text: code blocks.
        QPainter p(viewport());
        p.setRenderHint(QPainter::Antialiasing);
        QRectF run;
        auto flush = [&] {
            if (run.isNull()) return;
            p.setPen(Qt::NoPen);
            p.setBrush(k.codeBg.darker(Theme::dark() ? 70 : 102));
            p.drawRoundedRect(run.adjusted(-6, -2, 6, 2), Theme::radius, Theme::radius);
            run = QRectF();
        };
        for (QTextBlock b = first; b.isValid(); b = b.next()) {
            const QRectF r = layout->blockBoundingRect(b);
            if (r.top() > visible.bottom()) break;
            const auto *info = static_cast<BlockInfo *>(b.userData());
            const bool code = info && (info->kind == BlockInfo::Code || (info->kind == BlockInfo::Fence && live));
            if (code) {
                const QRectF rr(colLeft, r.top() + off.y(), colRight - colLeft, r.height());
                run = run.isNull() ? rr : run.united(rr);
            } else {
                flush();
            }
        }
        flush();
    }

    QTextEdit::paintEvent(e);

    if (!live) return;
    QPainter p(viewport());
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const qreal dpr = devicePixelRatioF();
    QHash<int, TableGeometry> tables;
    for (QTextBlock b = first; b.isValid(); b = b.next()) {
        const QRectF br = layout->blockBoundingRect(b);
        if (br.top() > visible.bottom()) break;
        const auto *info = static_cast<BlockInfo *>(b.userData());
        if (!info) continue;
        const QRectF r = br.translated(off);
        if (info->kind == BlockInfo::Table && !info->active) {
            // Rows run to the next line's top: the line height's leading is
            // not part of the block's rectangle.
            QRectF row = r;
            if (b.next().isValid()) row.setBottom(layout->blockBoundingRect(b.next()).top() + off.y());
            paintTableRow(p, b, row, tables);
            continue;
        }
        if (info->kind == BlockInfo::Quote && !info->active) {
            p.setPen(Qt::NoPen);
            p.setBrush(Theme::alpha(k.ivy, 0.35));
            p.drawRoundedRect(QRectF(r.left() + 2, r.top() + 5, 2, r.height() - 10), 1, 1);
        }
        if (info->active) continue;
        if (info->kind == BlockInfo::Rule) {
            p.setPen(QPen(k.line, 1));
            p.drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right() - 4, r.center().y()));
        }
        for (int pos : info->bullets) {
            const QRectF cr = charRect(b, pos);
            p.setPen(Qt::NoPen);
            p.setBrush(k.ink2);
            p.drawEllipse(QPointF(cr.center().x(), cr.center().y() + 1), 2.6, 2.6);
        }
        for (const auto &[pos, done] : info->tasks) {
            const QRectF cr = charRect(b, pos);
            const qreal s = std::min<qreal>(16, m_px);
            const QRectF box(cr.left() + 1, cr.center().y() - s / 2 + 1, s, s);
            p.setPen(done ? Qt::NoPen : QPen(k.faint, 1.4));
            p.setBrush(done ? k.ivy : Qt::transparent);
            p.drawRoundedRect(box, 3, 3);
            if (done) {
                QPainterPath tick;
                tick.moveTo(box.left() + s * 0.26, box.top() + s * 0.52);
                tick.lineTo(box.left() + s * 0.44, box.top() + s * 0.70);
                tick.lineTo(box.left() + s * 0.76, box.top() + s * 0.32);
                p.setPen(QPen(k.surface, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p.setBrush(Qt::NoBrush);
                p.drawPath(tick);
            }
        }
        if (info->kind == BlockInfo::Image && info->imageSize.isValid()) {
            const QRectF area(r.left(), r.top() + 6, info->imageSize.width(), info->imageSize.height());
            const QImage img = image ? image(info->imageId) : QImage();
            QPainterPath clip;
            clip.addRoundedRect(area, Theme::radius, Theme::radius);
            if (!img.isNull()) {
                p.save();
                p.setClipPath(clip);
                p.drawImage(area, img);
                p.restore();
            } else {
                p.setPen(Qt::NoPen);
                p.setBrush(k.sunken);
                p.drawPath(clip);
                p.setPen(k.faint);
                p.setFont(Theme::uiFont(12));
                p.drawText(area, Qt::AlignCenter, QStringLiteral("🖼"));
            }
            Q_UNUSED(dpr);
        }
    }
}

MarkdownEditor::TableGeometry MarkdownEditor::tableGeometry(const QTextBlock &start) const {
    TableGeometry g;
    QStringList rows;
    for (QTextBlock b = start; b.isValid(); b = b.next()) {
        const auto *info = static_cast<BlockInfo *>(b.userData());
        if (!info || info->kind != BlockInfo::Table || (b != start && info->tableRow == 0)) break;
        rows << b.text();
    }
    if (rows.size() < 2) return g;
    const QStringList head = md::tableCells(rows[0]);
    g.cols = int(std::max<qsizetype>(1, head.size()));
    g.align = md::tableAlignments(rows[1]);
    while (g.align.size() < g.cols) g.align << int(Qt::AlignLeft);
    const QFontMetricsF bold(Theme::uiFont(m_px - 2, QFont::Medium));
    const QFontMetricsF plain(Theme::uiFont(m_px - 2));
    const qreal pad = 12;
    QList<qreal> w(g.cols, 40);
    for (int i = 0; i < rows.size(); ++i) {
        if (i == 1) continue;
        const QStringList cells = md::tableCells(rows[i]);
        for (int c = 0; c < g.cols && c < cells.size(); ++c)
            w[c] = std::max(w[c], (i == 0 ? bold : plain).horizontalAdvance(md::plainInline(cells[c])) + 2 * pad + 1);
    }
    // Wide tables give up width in proportion, down to a readable minimum.
    const qreal avail = viewport()->width() - 2 * document()->documentMargin() - 1;
    qreal sum = 0;
    for (qreal x : w) sum += x;
    if (sum > avail)
        for (qreal &x : w) x = std::max<qreal>(56, x * avail / sum);
    g.x << 0;
    for (qreal x : w) g.x << g.x.last() + std::round(x);
    return g;
}

void MarkdownEditor::paintTableRow(QPainter &p, const QTextBlock &block, const QRectF &r, QHash<int, TableGeometry> &cache) const {
    const Tokens &k = Theme::t();
    const auto *info = static_cast<BlockInfo *>(block.userData());
    QTextBlock start = block;
    while (static_cast<BlockInfo *>(start.userData())->tableRow > 0 && start.previous().isValid() && start.previous().userData() &&
           static_cast<BlockInfo *>(start.previous().userData())->kind == BlockInfo::Table)
        start = start.previous();
    auto it = cache.find(start.blockNumber());
    if (it == cache.end()) it = cache.insert(start.blockNumber(), tableGeometry(start));
    const TableGeometry &g = *it;
    if (g.cols == 0 || g.x.size() < 2) return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    const qreal left = std::round(r.left());
    const qreal top = std::round(r.top());
    const qreal bottom = std::round(r.bottom());
    const qreal right = left + g.x.last();
    // As on the web: rules between rows, a quiet header, no grid.
    const QColor rule = Theme::dark() ? k.line.lighter(125) : k.line;
    if (info->tableRow == 1) {
        p.restore();
        return;
    }
    p.setPen(QPen(rule, 1));
    p.drawLine(QPointF(left, bottom), QPointF(right, bottom));
    const QStringList cells = md::tableCells(block.text());
    p.setFont(Theme::uiFont(m_px - 2, info->tableRow == 0 ? QFont::Medium : QFont::Normal));
    p.setPen(info->tableRow == 0 ? k.muted : k.ink);
    const QFontMetricsF fm(p.font());
    for (int c = 0; c < g.cols && c < cells.size(); ++c) {
        const QRectF cell(left + g.x[c] + 12, top, g.x[c + 1] - g.x[c] - 24, bottom - top);
        const QString text = fm.elidedText(md::plainInline(cells[c]), Qt::ElideRight, cell.width());
        p.drawText(cell, Qt::AlignVCenter | g.align.value(c, Qt::AlignLeft), text);
    }
    p.restore();
}

const LinkSpan *MarkdownEditor::linkAt(const QPoint &pos, QTextBlock *blockOut) const {
    const QTextCursor c = cursorForPosition(pos);
    const QTextBlock b = c.block();
    const auto *info = static_cast<BlockInfo *>(b.userData());
    if (!info) return nullptr;
    const int p = c.positionInBlock();
    for (const LinkSpan &l : info->links) {
        if (p > l.start && p < l.end) {
            // Only when the pointer is really over the text, not past the line end.
            const QRectF cr = charRect(b, std::max(l.start, p - 1));
            if (std::abs(cr.center().y() - pos.y()) > cr.height()) return nullptr;
            *blockOut = b;
            return &l;
        }
    }
    return nullptr;
}

int MarkdownEditor::taskAt(const QPoint &pos, QTextBlock *blockOut) const {
    const QTextCursor c = cursorForPosition(pos);
    const QTextBlock b = c.block();
    const auto *info = static_cast<BlockInfo *>(b.userData());
    if (!info || info->active || m_hl->mode() != MarkdownHighlighter::Live) return -1;
    for (const auto &[p, done] : info->tasks) {
        const QRectF cr = charRect(b, p);
        const QRectF hit(cr.left() - 4, cr.top(), 26, cr.height());
        if (hit.contains(pos)) {
            *blockOut = b;
            return p;
        }
    }
    return -1;
}

void MarkdownEditor::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && !isReadOnly()) {
        QTextBlock b;
        const int task = taskAt(e->pos(), &b);
        if (task >= 0) {
            QTextCursor c(document());
            c.setPosition(b.position() + task + 1);
            c.setPosition(b.position() + task + 2, QTextCursor::KeepAnchor);
            const bool done = c.selectedText() != QLatin1String(" ");
            c.insertText(done ? QStringLiteral(" ") : QStringLiteral("x"));
            return;
        }
    }
    if (e->button() == Qt::LeftButton) {
        QTextBlock b;
        if (const LinkSpan *l = linkAt(e->pos(), &b)) {
            const auto *info = static_cast<BlockInfo *>(b.userData());
            const bool rendered = m_hl->mode() == MarkdownHighlighter::Live && info && !info->active;
            if (rendered || (e->modifiers() & Qt::ControlModifier)) {
                emit linkActivated(l->target, l->wiki);
                return;
            }
        }
    }
    QTextEdit::mousePressEvent(e);
}

void MarkdownEditor::mouseMoveEvent(QMouseEvent *e) {
    QTextBlock b;
    bool hand = taskAt(e->pos(), &b) >= 0;
    if (!hand) {
        if (linkAt(e->pos(), &b)) {
            const auto *info = static_cast<BlockInfo *>(b.userData());
            hand = (m_hl->mode() == MarkdownHighlighter::Live && info && !info->active) || (e->modifiers() & Qt::ControlModifier);
        }
    }
    viewport()->setCursor(hand ? Qt::PointingHandCursor : Qt::IBeamCursor);
    QTextEdit::mouseMoveEvent(e);
}

void MarkdownEditor::focusInEvent(QFocusEvent *e) {
    QTextEdit::focusInEvent(e);
    updateActive();
}

void MarkdownEditor::focusOutEvent(QFocusEvent *e) {
    QTextEdit::focusOutEvent(e);
    // Show the line rendered once the editor is left.
    if (!m_completer->popup()->isVisible()) m_hl->setActive(-1, -1);
}

// --- typing ---

void MarkdownEditor::toggleFormat(const QString &mark) {
    QTextCursor c = textCursor();
    const int n = int(mark.size());
    c.beginEditBlock();
    if (!c.hasSelection()) {
        c.insertText(mark + mark);
        c.movePosition(QTextCursor::Left, QTextCursor::MoveAnchor, n);
    } else {
        const int s = c.selectionStart(), e = c.selectionEnd();
        const QString all = toPlainText();
        if (s >= n && all.mid(s - n, n) == mark && all.mid(e, n) == mark) {
            QTextCursor x(document());
            x.setPosition(e);
            x.setPosition(e + n, QTextCursor::KeepAnchor);
            x.removeSelectedText();
            x.setPosition(s - n);
            x.setPosition(s, QTextCursor::KeepAnchor);
            x.removeSelectedText();
            c.setPosition(s - n);
            c.setPosition(e - n, QTextCursor::KeepAnchor);
        } else {
            const QString sel = c.selectedText();
            c.insertText(mark + sel + mark);
            c.setPosition(s + n);
            c.setPosition(e + n, QTextCursor::KeepAnchor);
        }
    }
    c.endEditBlock();
    setTextCursor(c);
}

void MarkdownEditor::setHeading(int level) {
    static const QRegularExpression head(QStringLiteral("^#{1,6}\\s+"));
    QTextCursor c = textCursor();
    c.beginEditBlock();
    const int a = document()->findBlock(c.selectionStart()).blockNumber();
    const int b = document()->findBlock(c.selectionEnd()).blockNumber();
    for (int i = a; i <= b; ++i) {
        QTextBlock blk = document()->findBlockByNumber(i);
        const QString t = blk.text();
        const auto m = head.match(t);
        const QString want = level > 0 ? QString(level, u'#') + u' ' : QString();
        QTextCursor x(blk);
        x.setPosition(blk.position());
        x.setPosition(blk.position() + (m.hasMatch() ? int(m.capturedLength()) : 0), QTextCursor::KeepAnchor);
        x.insertText(m.hasMatch() && m.captured(0).trimmed().size() == level ? QString() : want);
    }
    c.endEditBlock();
}

void MarkdownEditor::toggleList(const QString &kind) {
    static const QRegularExpression list(QStringLiteral(R"(^(\s*)([-*+]\s+\[[ xX]\]\s+|[-*+]\s+|\d+[.)]\s+))"));
    QTextCursor c = textCursor();
    c.beginEditBlock();
    const int a = document()->findBlock(c.selectionStart()).blockNumber();
    const int b = document()->findBlock(c.selectionEnd()).blockNumber();
    int n = 1;
    for (int i = a; i <= b; ++i) {
        QTextBlock blk = document()->findBlockByNumber(i);
        const QString t = blk.text();
        const auto m = list.match(t);
        QString want = kind == QLatin1String("1.") ? QString::number(n++) + QStringLiteral(". ") : kind + u' ';
        const bool same = m.hasMatch() && ((kind == QLatin1String("1.") && m.captured(2).front().isDigit()) ||
                                           (kind == QLatin1String("- [ ]") && m.captured(2).contains(u'[')) ||
                                           (kind == QLatin1String("-") && !m.captured(2).contains(u'[') && !m.captured(2).front().isDigit()));
        QTextCursor x(blk);
        const int indent = m.hasMatch() ? int(m.capturedLength(1)) : 0;
        x.setPosition(blk.position() + indent);
        x.setPosition(blk.position() + (m.hasMatch() ? int(m.capturedEnd()) : 0), QTextCursor::KeepAnchor);
        x.insertText(same ? QString() : want);
    }
    c.endEditBlock();
}

bool MarkdownEditor::continueList() {
    static const QRegularExpression list(QStringLiteral(R"(^(\s*)([-*+]\s+\[[ xX]\]\s+|[-*+]\s+|(\d+)([.)])\s+|>\s?))"));
    QTextCursor c = textCursor();
    if (c.hasSelection()) return false;
    const QString line = c.block().text();
    const auto m = list.match(line);
    if (!m.hasMatch() || c.positionInBlock() < m.capturedEnd()) return false;
    if (line.mid(m.capturedEnd()).trimmed().isEmpty()) {
        // Enter on an empty item ends the list.
        c.beginEditBlock();
        c.movePosition(QTextCursor::StartOfBlock);
        c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        c.removeSelectedText();
        c.endEditBlock();
        return true;
    }
    QString next = m.captured(1);
    const QString mark = m.captured(2);
    if (!m.captured(3).isEmpty()) next += QString::number(m.captured(3).toInt() + 1) + m.captured(4) + u' ';
    else if (mark.contains(u'[')) next += mark.left(mark.indexOf(u'[')) + QStringLiteral("[ ] ");
    else next += mark;
    c.beginEditBlock();
    c.insertBlock();
    c.insertText(next);
    c.endEditBlock();
    setTextCursor(c);
    ensureCursorVisible();
    return true;
}

void MarkdownEditor::keyPressEvent(QKeyEvent *e) {
    if (m_completer->popup()->isVisible()) {
        switch (e->key()) {
        case Qt::Key_Enter:
        case Qt::Key_Return:
        case Qt::Key_Escape:
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
            e->ignore();
            return;
        default:
            break;
        }
    }
    const auto mods = e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier);
    const bool ctrl = mods & Qt::ControlModifier;
    if (!isReadOnly() && ctrl) {
        const bool shift = mods & Qt::ShiftModifier;
        const bool alt = mods & Qt::AltModifier;
        if (!shift && !alt) {
            if (e->key() == Qt::Key_B) return toggleFormat(QStringLiteral("**"));
            if (e->key() == Qt::Key_I) return toggleFormat(QStringLiteral("*"));
            if (e->key() == Qt::Key_E) return toggleFormat(QStringLiteral("`"));
        }
        if (shift && !alt) {
            if (e->key() == Qt::Key_X) return toggleFormat(QStringLiteral("~~"));
            if (e->key() == Qt::Key_K) {
                insertPlainText(QStringLiteral("[[]]"));
                QTextCursor c = textCursor();
                c.movePosition(QTextCursor::Left, QTextCursor::MoveAnchor, 2);
                setTextCursor(c);
                updateCompletion();
                return;
            }
            // Shift+7/8/9 arrive as the shifted characters on most layouts.
            const int key = e->key();
            if (key == Qt::Key_7 || key == Qt::Key_Ampersand) return toggleList(QStringLiteral("1."));
            if (key == Qt::Key_8 || key == Qt::Key_Asterisk) return toggleList(QStringLiteral("-"));
            if (key == Qt::Key_9 || key == Qt::Key_ParenLeft) return toggleList(QStringLiteral("- [ ]"));
        }
        if (alt && !shift) {
            if (e->key() == Qt::Key_1) return setHeading(1);
            if (e->key() == Qt::Key_2) return setHeading(2);
            if (e->key() == Qt::Key_3) return setHeading(3);
            if (e->key() == Qt::Key_0) return setHeading(0);
        }
    }
    if (!isReadOnly() && (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && !mods) {
        if (continueList()) return;
    }
    if (!isReadOnly() && (e->key() == Qt::Key_Tab || e->key() == Qt::Key_Backtab)) {
        // Indent or outdent list items; plain Tab inserts spaces.
        QTextCursor c = textCursor();
        const bool out = e->key() == Qt::Key_Backtab || (mods & Qt::ShiftModifier);
        static const QRegularExpression item(QStringLiteral(R"(^\s*([-*+]|\d+[.)])\s)"));
        if (item.match(c.block().text()).hasMatch() || c.hasSelection() || out) {
            c.beginEditBlock();
            const int a = document()->findBlock(c.selectionStart()).blockNumber();
            const int b = document()->findBlock(c.selectionEnd()).blockNumber();
            for (int i = a; i <= b; ++i) {
                QTextBlock blk = document()->findBlockByNumber(i);
                QTextCursor x(blk);
                if (out) {
                    int n = 0;
                    while (n < 2 && n < blk.text().size() && blk.text()[n] == u' ') n++;
                    x.setPosition(blk.position() + n, QTextCursor::KeepAnchor);
                    x.removeSelectedText();
                } else {
                    x.insertText(QStringLiteral("  "));
                }
            }
            c.endEditBlock();
            return;
        }
        insertPlainText(QStringLiteral("    "));
        return;
    }
    QTextEdit::keyPressEvent(e);
    if (!e->text().isEmpty() || e->key() == Qt::Key_Backspace) updateCompletion();
}

void MarkdownEditor::updateCompletion() {
    static const QRegularExpression open(QStringLiteral(R"(\[\[([^\[\]|#\n]*)$)"));
    const QTextCursor c = textCursor();
    const QString before = c.block().text().left(c.positionInBlock());
    const auto m = open.match(before);
    if (!m.hasMatch() || m_titles->rowCount() == 0) {
        m_completer->popup()->hide();
        return;
    }
    m_completer->setCompletionPrefix(m.captured(1));
    if (m_completer->completionCount() == 0) {
        m_completer->popup()->hide();
        return;
    }
    QRect r = cursorRect();
    r.translate(viewportMargins().left(), 4);
    r.setWidth(std::min(360, std::max(240, m_completer->popup()->sizeHintForColumn(0) + 24)));
    m_completer->popup()->setFont(Theme::uiFont(13));
    m_completer->complete(r);
}

void MarkdownEditor::insertCompletion(const QString &title) {
    static const QRegularExpression open(QStringLiteral(R"(\[\[([^\[\]|#\n]*)$)"));
    QTextCursor c = textCursor();
    const QString before = c.block().text().left(c.positionInBlock());
    const auto m = open.match(before);
    if (!m.hasMatch()) return;
    c.beginEditBlock();
    c.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor, int(m.capturedLength(1)));
    c.insertText(title);
    const QString after = c.block().text().mid(c.positionInBlock());
    if (after.startsWith(QLatin1String("]]"))) c.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, 2);
    else c.insertText(QStringLiteral("]]"));
    c.endEditBlock();
    setTextCursor(c);
}

void MarkdownEditor::insertBlockText(const QString &markdown) {
    QTextCursor c = textCursor();
    c.beginEditBlock();
    if (c.hasSelection()) c.removeSelectedText();
    if (!c.block().text().isEmpty()) {
        if (c.positionInBlock() > 0) c.movePosition(QTextCursor::EndOfBlock);
        c.insertBlock();
    }
    c.insertText(markdown);
    c.endEditBlock();
    setTextCursor(c);
    ensureCursorVisible();
}

// --- paste and drop ---

bool MarkdownEditor::canInsertFromMimeData(const QMimeData *source) const {
    return source->hasImage() || source->hasUrls() || source->hasText();
}

void MarkdownEditor::insertFromMimeData(const QMimeData *source) {
    if (isReadOnly()) return;
    if (source->hasUrls()) {
        QStringList files;
        for (const QUrl &u : source->urls())
            if (u.isLocalFile() && QFileInfo(u.toLocalFile()).isFile()) files << u.toLocalFile();
        if (!files.isEmpty()) {
            emit filesDropped(files);
            return;
        }
    }
    if (source->hasImage() && !source->hasText()) {
        const QImage img = qvariant_cast<QImage>(source->imageData());
        if (!img.isNull()) {
            emit imagePasted(img);
            return;
        }
    }
    if (source->hasText()) {
        // Always plain text: the document is Markdown, never rich text.
        QString t = source->text();
        t.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        t.replace(u'\r', u'\n');
        insertPlainText(t);
        ensureCursorVisible();
    }
}

void MarkdownEditor::dragEnterEvent(QDragEnterEvent *e) {
    if (e->mimeData()->hasUrls() && !isReadOnly()) e->acceptProposedAction();
    else QTextEdit::dragEnterEvent(e);
}

void MarkdownEditor::dragMoveEvent(QDragMoveEvent *e) {
    if (e->mimeData()->hasUrls() && !isReadOnly()) {
        setTextCursor(cursorForPosition(e->position().toPoint()));
        e->acceptProposedAction();
    } else {
        QTextEdit::dragMoveEvent(e);
    }
}

void MarkdownEditor::dropEvent(QDropEvent *e) {
    if (e->mimeData()->hasUrls() && !isReadOnly()) {
        setTextCursor(cursorForPosition(e->position().toPoint()));
        insertFromMimeData(e->mimeData());
        e->acceptProposedAction();
        return;
    }
    QTextEdit::dropEvent(e);
}

#include "ui/editor.h"

#include "core/i18n.h"
#include "core/insertitems.h"
#include "core/markdown.h"
#include "core/tasks.h"
#include "ui/remoteimages.h"
#include "ui/taskpicker.h"
#include "ui/theme.h"
#include "ui/render.h"
#include "ui/widgets.h"

#include <algorithm>
#include <cmath>

#include <QAbstractItemView>
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QCompleter>
#include <QClipboard>
#include <QTextDocumentFragment>
#include <QFileInfo>
#include <QContextMenuEvent>
#include <QFrame>
#include <QKeyEvent>
#include <QInputDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QCollator>
#include <QDateTime>
#include <QUrl>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStringListModel>
#include <QStyledItemDelegate>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QVBoxLayout>
#include <QVariantAnimation>

namespace {

enum PopupRole {
    RoleId = Qt::UserRole + 1,
    RoleIcon,
    RoleHint,
    RoleDesc,
    RoleIsHeader,
    RoleDueDate,
    RoleDueTime,
};

bool inFrontmatter(const QTextBlock &block) {
    const QTextDocument *doc = block.document();
    if (!doc || block.blockNumber() <= 0) return false;
    const QTextBlock first = doc->firstBlock();
    if (first.text().trimmed() != QLatin1String("---")) return false;
    int scanned = 0;
    for (QTextBlock b = first.next(); b.isValid() && scanned < 80; b = b.next(), ++scanned) {
        const QString t = b.text().trimmed();
        if (t == QLatin1String("---") || t == QLatin1String("..."))
            return block.blockNumber() < b.blockNumber();
    }
    return false;
}

bool inInlineCode(const QString &lineBeforeCursor) {
    int ticks = 0;
    for (qsizetype i = 0; i < lineBeforeCursor.size(); ++i) {
        if (lineBeforeCursor[i] == u'`' && (i == 0 || lineBeforeCursor[i - 1] != u'\\')) ++ticks;
    }
    return (ticks % 2) == 1;
}

class InlinePopupDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const override {
        if (index.data(RoleIsHeader).toBool()) return QSize(260, 24);
        return QSize(260, 34);
    }

    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const override {
        const Tokens &k = Theme::t();
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRect r = opt.rect.adjusted(4, 1, -4, -1);
        if (index.data(RoleIsHeader).toBool()) {
            p->setFont(Theme::uiFont(11, QFont::Medium));
            p->setPen(k.muted);
            p->drawText(r.adjusted(8, 4, -8, 0), Qt::AlignVCenter | Qt::AlignLeft, index.data(Qt::DisplayRole).toString());
            p->restore();
            return;
        }
        const bool selected = opt.state.testFlag(QStyle::State_Selected);
        const bool hover = opt.state.testFlag(QStyle::State_MouseOver);
        if (selected || hover) {
            p->setPen(Qt::NoPen);
            p->setBrush(selected ? Theme::alpha(k.ivy, Theme::dark() ? 0.22 : 0.12) : k.hover);
            p->drawRoundedRect(r, 6, 6);
        }
        const QString icon = index.data(RoleIcon).toString();
        const QString label = index.data(Qt::DisplayRole).toString();
        const QString hint = index.data(RoleHint).toString();

        const QRect badge(r.left() + 6, r.center().y() - 10, 24, 20);
        p->setPen(Qt::NoPen);
        p->setBrush(selected ? Theme::alpha(k.ivy, 0.16) : k.sunken);
        p->drawRoundedRect(badge, 4, 4);
        p->setFont(Theme::mono(11));
        p->setPen(selected ? k.ivy : k.muted);
        p->drawText(badge, Qt::AlignCenter, icon);

        const int rightPad = 8;
        int hintW = 0;
        if (!hint.isEmpty()) {
            p->setFont(Theme::mono(11));
            hintW = QFontMetrics(p->font()).horizontalAdvance(hint) + 8;
            p->setPen(k.faint);
            p->drawText(QRect(r.right() - rightPad - hintW, r.top(), hintW, r.height()),
                        Qt::AlignVCenter | Qt::AlignRight, hint);
        }
        p->setFont(Theme::uiFont(13, selected ? QFont::Medium : QFont::Normal));
        p->setPen(selected ? k.ink : k.ink2);
        const QRect textRect(badge.right() + 8, r.top(), std::max(40, r.right() - badge.right() - 16 - hintW), r.height());
        p->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                    QFontMetrics(p->font()).elidedText(label, Qt::ElideRight, textRect.width()));
        p->restore();
    }
};

} // namespace

MarkdownEditor::MarkdownEditor(QWidget *parent)
    : QTextEdit(parent), m_hl(new MarkdownHighlighter(document())), m_completer(new QCompleter(this)),
      m_titles(new QStringListModel(this)) {
    m_cellEdit = new QLineEdit(viewport());
    m_cellEdit->setObjectName(QStringLiteral("tableCellEditor"));
    m_cellEdit->hide();
    m_cellEdit->installEventFilter(this);
    passAppShortcuts(m_cellEdit);
    connect(m_cellEdit, &QLineEdit::textEdited, this, [this] { saveTableCell(); });
    connect(m_cellEdit, &QLineEdit::editingFinished, this, [this] { closeTableCell(true); });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { positionTableCell(); });
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, [this] { positionTableCell(); });
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
    setPlaceholderText(T("开始书写… 输入 / 插入元素，输入 [[ 链接其他笔记"));

    m_popup = new QFrame(this);
    m_popup->setObjectName(QStringLiteral("editorInlinePopup"));
    m_popup->setFocusPolicy(Qt::NoFocus);
    m_popup->hide();
    auto *popLayout = new QVBoxLayout(m_popup);
    popLayout->setContentsMargins(4, 4, 4, 4);
    popLayout->setSpacing(0);
    m_popupList = new QListWidget(m_popup);
    m_popupList->setFocusPolicy(Qt::NoFocus);
    m_popupList->setFrameShape(QFrame::NoFrame);
    m_popupList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_popupList->setMouseTracking(true);
    m_popupList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_popupList->setItemDelegate(new InlinePopupDelegate(m_popupList));
    popLayout->addWidget(m_popupList);
    auto restylePopup = [this] {
        const Tokens &k = Theme::t();
        m_popup->setStyleSheet(QStringLiteral(
            "#editorInlinePopup{background:%1;border:1px solid %2;border-radius:8px;}"
            "#editorInlinePopup QListWidget{background:transparent;border:none;outline:none;}")
            .arg(k.surface.name(), k.line.name()));
    };
    restylePopup();
    connect(m_popupList, &QListWidget::itemClicked, this, &MarkdownEditor::acceptInlinePopupItem);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        if (m_popup->isVisible()) positionInlinePopup();
    });

    m_hl->wikiExists = [this](const QString &t) { return !wikiExists || wikiExists(t); };
    m_hl->imageLoader = [this](const QString &id) { return imageFor(id); };
    m_hl->imageSize = [this](const QString &id) -> QSize {
        const QImage img = imageFor(id);
        if (!img.isNull()) return img.size() / std::max(1.0, img.devicePixelRatio());
        return imageMeta && !RemoteImages::isRemote(id) ? imageMeta(id) : QSize();
    };
    connect(RemoteImages::instance(), &RemoteImages::ready, this, &MarkdownEditor::refreshAttachment);
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
        if (m_popup->isVisible()) updateInlinePopup();
        const int line = textCursor().blockNumber();
        if (line != m_lastLine) {
            m_lastLine = line;
            emit cursorLineChanged(line);
        }
    });
    connect(this, &QTextEdit::selectionChanged, this, &MarkdownEditor::updateActive);
    m_reveal.setStartValue(1.0);
    m_reveal.setEndValue(0.0);
    m_reveal.setDuration(150);
    m_reveal.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_reveal, &QVariantAnimation::valueChanged, viewport(), qOverload<>(&QWidget::update));
    connect(&m_reveal, &QVariantAnimation::finished, this, [this] {
        m_revealShot = QPixmap();
        viewport()->update();
    });
    // A scrolled or edited view no longer matches the snapshot.
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        if (m_reveal.state() == QAbstractAnimation::Running) {
            m_reveal.stop();
            m_revealShot = QPixmap();
        }
    });
    connect(document(), &QTextDocument::contentsChange, this, [this](int pos, int removed, int added) {
        if (m_relayout) return;
        // The highlighter has run; lay out the lines whose kind changed.
        if (!m_undoing && document()->isUndoRedoEnabled()) {
            QList<int> blocks = m_hl->takeChanged();
            const int a = document()->findBlock(pos).blockNumber();
            const int b = document()->findBlock(pos + added).blockNumber();
            for (int i = std::max(0, a); i <= b; ++i) blocks << i;
            relayout(blocks, true);
        } else if (!document()->isUndoRedoEnabled()) {
            m_hl->takeChanged();
        }
        if (m_loading || (removed == 0 && added == 0)) return;
        emit edited();
    });
    connect(Theme::instance(), &Theme::changed, this, [this, restylePopup] {
        QPalette p = palette();
        p.setColor(QPalette::Base, Theme::t().surface);
        p.setColor(QPalette::Window, Theme::t().surface);
        p.setColor(QPalette::Text, Theme::t().ink);
        setPalette(p);
        restylePopup();
        viewport()->update();
    });
    QPalette p = palette();
    p.setColor(QPalette::Base, Theme::t().surface);
    p.setColor(QPalette::Window, Theme::t().surface);
    setPalette(p);
    setFontSize(16);
}

MarkdownEditor::~MarkdownEditor() {
    // QWidget tears down the focused child after derived members are gone.
    // Its focus-out/finished signals must not enter the editor at that point.
    m_cellEdit->disconnect(this);
    m_cellEdit->removeEventFilter(this);
    m_cellEdit->hide();
}

void MarkdownEditor::setMarkdown(const QString &text) {
    hideInlinePopup(false);
    closeTableCell(false);
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

QTextBlockFormat MarkdownEditor::blockFormatFor(const QTextBlock &b) const {
    // Spacing and indents by kind: code sits inset on its panel, wrapped
    // lines of lists and quotes hang under their text.
    const bool live = m_hl->mode() == MarkdownHighlighter::Live;
    const auto *info = static_cast<BlockInfo *>(b.userData());
    QTextBlockFormat f;
    int lh = live ? 175 : 165;
    qreal left = 0, indent = 0;
    if (info && (info->kind == BlockInfo::Code || info->kind == BlockInfo::Fence)) {
        left = 16;
        lh = 150;
    } else if (info && info->hang > 0) {
        left = std::round(info->hang);
        indent = -left;
    }
    f.setLineHeight(lh, QTextBlockFormat::ProportionalHeight);
    f.setLeftMargin(left);
    f.setRightMargin(info && (info->kind == BlockInfo::Code || info->kind == BlockInfo::Fence) ? 16 : 0);
    f.setTextIndent(indent);
    return f;
}

void MarkdownEditor::relayout(const QList<int> &blocks, bool afterEdit) {
    // Block formats follow the text. After an edit they land on the undo
    // stack as a step of their own, which undo and redo take together with
    // the edit before it (see undo()).
    QList<QPair<int, QTextBlockFormat>> changes;
    for (int n : blocks) {
        const QTextBlock b = document()->findBlockByNumber(n);
        if (!b.isValid()) continue;
        const QTextBlockFormat want = blockFormatFor(b);
        const QTextBlockFormat now = b.blockFormat();
        if (now.lineHeight() != want.lineHeight() || now.lineHeightType() != want.lineHeightType() || now.leftMargin() != want.leftMargin() ||
            now.rightMargin() != want.rightMargin() || now.textIndent() != want.textIndent())
            changes.append({b.position(), want});
    }
    if (changes.isEmpty()) return;
    m_relayout = true;
    QTextCursor c(document());
    c.beginEditBlock();
    for (const auto &[pos, f] : std::as_const(changes)) {
        c.setPosition(pos);
        c.mergeBlockFormat(f);
    }
    c.endEditBlock();
    m_relayout = false;
    Q_UNUSED(afterEdit);
}


void MarkdownEditor::undo() {
    closeTableCell(true);
    // Mode/font/layout commands share Qt's stack; one user undo must
    // reach a text change even after any number of presentation changes.
    QTextDocument *d = document();
    m_undoing = true;
    m_loading = true;
    const QString before = toPlainText();
    QTextCursor c = textCursor();
    do { d->undo(&c); } while (toPlainText() == before && d->isUndoAvailable());
    setTextCursor(c);
    m_undoing = false;
    m_loading = false;
    m_hl->takeChanged();
    if (toPlainText() != before) emit edited();
}

void MarkdownEditor::redo() {
    closeTableCell(true);
    QTextDocument *d = document();
    m_undoing = true;
    m_loading = true;
    const QString before = toPlainText();
    QTextCursor c = textCursor();
    do { d->redo(&c); } while (toPlainText() == before && d->isRedoAvailable());
    // Qt exposes no command type. Probe the next command and immediately
    // put it back if it edits text; consume formatting belonging to this edit.
    const QString after = toPlainText();
    while (d->isRedoAvailable()) {
        const QTextCursor previous = c;
        d->redo(&c);
        if (toPlainText() != after) {
            d->undo(&c);
            c = previous;
            break;
        }
    }
    setTextCursor(c);
    m_undoing = false;
    m_loading = false;
    m_hl->takeChanged();
    if (toPlainText() != before) emit edited();
}

void MarkdownEditor::applyLineHeight() {
    // Callers keep this off the undo stack; new lines inherit the formats.
    m_hl->takeChanged();
    QList<int> all;
    for (int i = 0; i < document()->blockCount(); ++i) all << i;
    relayout(all, false);
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
    closeTableCell(true);
    if (m_hl->mode() == m) return;
    m_loading = true;
    m_hl->setMode(m);
    setFontSize(m_px);
    applyLineHeight();
    m_loading = false;
    updateActive();
    viewport()->update();
}

void MarkdownEditor::setFontSize(int px) {
    const bool loading = m_loading;
    m_loading = true;
    m_px = std::clamp(px, 13, 22);
    const bool live = m_hl->mode() == MarkdownHighlighter::Live;
    QFont f = live ? Theme::uiFont(m_px) : Theme::mono(std::max(13, m_px - 2));
    document()->setDefaultFont(f);
    setFont(f);
    m_hl->setFontSize(m_px);
    m_hl->rehighlight();
    if (!document()->isEmpty()) applyLineHeight();
    m_loading = loading;
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
    if (cw != before) {
        // Image sizes depend on the column width.
        m_hl->setProperty("cw", cw);
        m_hl->rehighlight();
    }
    positionTableCell();
    if (m_popup->isVisible()) positionInlinePopup();
}

void MarkdownEditor::setWikiTitles(const QStringList &titles) { m_titles->setStringList(titles); }

QImage MarkdownEditor::imageFor(const QString &id) const {
    if (RemoteImages::isRemote(id)) return RemoteImages::instance()->image(id);
    return image ? image(id) : QImage();
}

void MarkdownEditor::refreshAttachment(const QString &id) {
    for (QTextBlock b = document()->begin(); b.isValid(); b = b.next())
        if (b.text().contains(id) || (b.userData() && (static_cast<BlockInfo *>(b.userData())->kind == BlockInfo::ImageParagraph || static_cast<BlockInfo *>(b.userData())->imageId == id))) m_hl->rehighlightBlock(b);
    viewport()->update();
}

void MarkdownEditor::updateActive() {
    // Only the line being edited shows its source.
    if (!hasFocus() && !m_completer->popup()->isVisible() && !inlinePopupVisible() && !textCursor().hasSelection()) {
        setActiveLines(-1, -1);
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
    if (m_hl->mode() == MarkdownHighlighter::Live && !c.hasSelection() && inTable(first)) {
        setActiveLines(-1, -1);
        return;
    }
    if (inTable(first))
        while (inTable(first.previous()) && static_cast<BlockInfo *>(first.userData())->tableRow > 0) first = first.previous();
    if (inTable(last))
        while (inTable(last.next()) && static_cast<BlockInfo *>(last.next().userData())->tableRow > 0) last = last.next();
    setActiveLines(first.blockNumber(), last.blockNumber());
}

void MarkdownEditor::setActiveLines(int first, int last) {
    if (first == m_hl->activeFirst() && last == m_hl->activeLast()) return;
    // Revealing or hiding a line's syntax cross-fades instead of cutting —
    // but not while typing, when the text itself is moving.
    const bool edited = document()->revision() != m_seenRevision;
    m_seenRevision = document()->revision();
    auto marked = [this](int a, int b) {
        static const QRegularExpression syntax(QStringLiteral(R"([#>*_`~=|\[\]!]|^\s*([-+]|\d+[.)])\s)"));
        for (int i = std::max(0, a); a >= 0 && i <= b; ++i) {
            const QTextBlock blk = document()->findBlockByNumber(i);
            const auto *info = static_cast<BlockInfo *>(blk.userData());
            if ((info && info->kind != BlockInfo::Text) || syntax.match(blk.text()).hasMatch()) return true;
        }
        return false;
    };
    if (!edited && m_hl->mode() == MarkdownHighlighter::Live && isVisible() && !Theme::reducedMotion() &&
        (marked(m_hl->activeFirst(), m_hl->activeLast()) || marked(first, last))) {
        m_reveal.stop();
        m_revealShot = viewport()->grab();
        m_reveal.start();
    }
    m_hl->setActive(first, last);
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
            // A soft panel with a hairline edge.
            p.setPen(QPen(Theme::dark() ? QColor(255, 255, 255, 14) : QColor(16, 24, 32, 12), 1));
            p.setBrush(k.codeBg.darker(Theme::dark() ? 70 : 102));
            p.drawRoundedRect(run.adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
            run = QRectF();
        };
        for (QTextBlock b = first; b.isValid(); b = b.next()) {
            const QRectF r = layout->blockBoundingRect(b);
            if (r.top() > visible.bottom()) break;
            const auto *info = static_cast<BlockInfo *>(b.userData());
            const bool code = info && (info->kind == BlockInfo::Code || info->kind == BlockInfo::Fence);
            if (code) {
                // To the next line's top: line leading is outside the block rect.
                const qreal bottom = b.next().isValid() ? layout->blockBoundingRect(b.next()).top() : r.bottom();
                const QRectF rr(colLeft + 4, r.top() + off.y(), colRight - colLeft - 8, bottom - r.top());
                run = run.isNull() ? rr : run.united(rr);
            } else {
                flush();
            }
        }
        flush();
    }

    QTextEdit::paintEvent(e);
    if (live) paintRendered(first);
    if (!m_revealShot.isNull() && m_reveal.state() == QAbstractAnimation::Running) {
        QPainter p(viewport());
        p.setOpacity(m_reveal.currentValue().toReal());
        p.drawPixmap(0, 0, m_revealShot);
    }
}

void MarkdownEditor::paintRendered(QTextBlock first) {
    const Tokens &k = Theme::t();
    QAbstractTextDocumentLayout *layout = document()->documentLayout();
    const QPointF off(-horizontalScrollBar()->value(), -verticalScrollBar()->value());
    const QRectF visible = QRectF(viewport()->rect()).translated(-off);
    QPainter p(viewport());
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const qreal dpr = devicePixelRatioF();
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
            paintTableRow(p, b, row);
            continue;
        }
        if (info->kind == BlockInfo::Quote && !info->active) {
            // One bar down the rendered quote, joined across its lines; a
            // line being edited shows its ">" instead.
            const QTextLayout *tl = b.layout();
            const auto *prevInfo = b.previous().isValid() ? static_cast<BlockInfo *>(b.previous().userData()) : nullptr;
            const auto *nextInfo = b.next().isValid() ? static_cast<BlockInfo *>(b.next().userData()) : nullptr;
            const bool joinUp = prevInfo && prevInfo->kind == BlockInfo::Quote && !prevInfo->active;
            const bool joinDown = nextInfo && nextInfo->kind == BlockInfo::Quote && !nextInfo->active;
            qreal top = r.top(), bottom = r.bottom();
            if (tl && tl->lineCount() > 0) {
                const QTextLine last = tl->lineAt(tl->lineCount() - 1);
                top = r.top() + tl->lineAt(0).y() + (joinUp ? 0 : 4);
                bottom = joinDown ? layout->blockBoundingRect(b.next()).top() + off.y()
                                  : r.top() + last.y() + last.ascent() + last.descent() + 4;
            }
            p.setPen(Qt::NoPen);
            p.setBrush(Theme::alpha(k.ivy, 0.4));
            p.drawRect(QRectF(r.left() + 1, top, 2.5, bottom - top));
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
            p.drawEllipse(QPointF(cr.left() + 4, cr.center().y() + 1), 2.6, 2.6);
        }
        for (qsizetype t = 0; t < info->tasks.size(); ++t) {
            const bool done = info->tasks[t].second;
            const QRectF cr = charRect(b, info->taskBoxes.value(t, info->tasks[t].first));
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
        if (info->kind == BlockInfo::ImageParagraph && info->imageDocument) {
            p.save();
            p.translate(r.left(), r.top() + 6);
            info->imageDocument->drawContents(&p);
            p.restore();
        }
        if (info->kind == BlockInfo::Image && info->imageSize.isValid()) {
            const QRectF area(r.left(), r.top() + 6, info->imageSize.width(), info->imageSize.height());
            const QImage img = imageFor(info->imageId);
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
                p.drawText(area, Qt::AlignCenter, RemoteImages::isRemote(info->imageId) && RemoteImages::instance()->failed(info->imageId) ? T("图片加载失败，右键可重试") : T("图片加载中…"));
            }
            Q_UNUSED(dpr);
        }
    }
}

QTextCursor MarkdownEditor::tableRange(const QTextBlock &block) const {
    const auto *info = block.isValid() ? static_cast<BlockInfo *>(block.userData()) : nullptr;
    if (!info || info->kind != BlockInfo::Table) return {};
    QTextBlock first = block;
    while (first.previous().isValid() && static_cast<BlockInfo *>(first.userData())->tableRow > 0) first = first.previous();
    QTextBlock last = first.next();
    while (last.next().isValid()) {
        const auto *next = static_cast<BlockInfo *>(last.next().userData());
        if (!next || next->kind != BlockInfo::Table || next->tableRow == 0) break;
        last = last.next();
    }
    QTextCursor range(first);
    range.setPosition(last.position() + int(last.text().size()), QTextCursor::KeepAnchor);
    return range;
}

bool MarkdownEditor::tableCellAt(const QPoint &point, int *row, int *column, QTextBlock *header) const {
    const QTextBlock block = cursorForPosition(point).block();
    const auto *info = static_cast<BlockInfo *>(block.userData());
    if (!info || info->kind != BlockInfo::Table || info->tableRow == 1) return false;
    const auto geometry = m_hl->tableGeometry(block);
    const QRectF rect = document()->documentLayout()->blockBoundingRect(block).translated(-horizontalScrollBar()->value(), -verticalScrollBar()->value());
    const qreal x = point.x() - rect.left();
    if (point.y() < rect.top() || point.y() > rect.top() + geometry.rowHeight.value(info->tableRow)) return false;
    for (int col = 0; col < geometry.cols; ++col) {
        if (x < geometry.x[col] || x >= geometry.x[col + 1]) continue;
        *row = info->tableRow > 1 ? info->tableRow - 1 : 0;
        *column = col;
        *header = document()->findBlockByNumber(geometry.start);
        return true;
    }
    return false;
}

bool MarkdownEditor::editTableCell(int row, int column) {
    if (isReadOnly() || mode() != MarkdownHighlighter::Live) return false;
    closeTableCell(true);
    QTextCursor range = tableRange(textCursor().block());
    if (range.isNull()) return false;
    const md::Table table = md::parseTable(range.selectedText().replace(QChar::ParagraphSeparator, u'\n'));
    if (!table.valid() || row < 0 || row >= table.rows.size() || column < 0 || column >= table.align.size()) return false;
    m_cellAnchor = QTextCursor(document()->findBlock(range.selectionStart()));
    m_cellAnchor.setKeepPositionOnInsert(true);
    m_cellRow = row;
    m_cellColumn = column;
    m_cellSaveStarted = false;
    m_cellSaveRevision = -1;
    m_cellEdit->setText(table.rows[row][column]);
    m_cellEdit->setFont(Theme::uiFont(m_px - 2));
    setActiveLines(-1, -1);
    m_cellEdit->show();
    positionTableCell();
    m_cellEdit->setFocus();
    m_cellEdit->selectAll();
    return true;
}

void MarkdownEditor::positionTableCell() {
    if (m_cellEdit->isHidden() || m_cellAnchor.isNull()) return;
    const QTextBlock header = m_cellAnchor.block();
    const auto geometry = m_hl->tableGeometry(header);
    const int line = header.blockNumber() + m_cellRow + (m_cellRow > 0 ? 1 : 0);
    const QTextBlock block = document()->findBlockByNumber(line);
    if (!block.isValid() || m_cellColumn >= geometry.cols) return;
    const QRectF rect = document()->documentLayout()->blockBoundingRect(block).translated(-horizontalScrollBar()->value(), -verticalScrollBar()->value());
    m_cellEdit->setGeometry(qRound(rect.left() + geometry.x[m_cellColumn] + 2), qRound(rect.top() + 2),
                            std::max(24, qRound(geometry.x[m_cellColumn + 1] - geometry.x[m_cellColumn] - 4)),
                            std::max(26, qRound(geometry.rowHeight.value(m_cellRow + (m_cellRow > 0 ? 1 : 0)) - 4)));
    m_cellEdit->raise();
}

void MarkdownEditor::commitPendingCell() { saveTableCell(); }

void MarkdownEditor::saveTableCell() {
    if (m_closingCell || m_cellEdit->isHidden() || isReadOnly() || m_cellAnchor.isNull()) return;
    QTextCursor range = tableRange(m_cellAnchor.block());
    const QString before = range.selectedText().replace(QChar::ParagraphSeparator, u'\n');
    md::Table table = md::parseTable(before);
    if (!table.valid() || m_cellRow >= table.rows.size() || m_cellColumn >= table.align.size()) return;
    const QString value = m_cellEdit->text();
    if (table.rows[m_cellRow][m_cellColumn] == value) return;
    table.rows[m_cellRow][m_cellColumn] = value;
    const QString formatted = md::formatTable(table);
    if (formatted == before) return;
    m_closingCell = true;
    // A cell writes through immediately, so normal local saving/sync applies.
    // Subsequent typing in this cell remains one undo step unless another
    // document edit intervened (e.g. a remote change).
    if (m_cellSaveStarted && m_cellSaveRevision == document()->revision()) range.joinPreviousEditBlock();
    else range.beginEditBlock();
    range.insertText(formatted);
    range.endEditBlock();
    m_cellSaveRevision = document()->revision();
    m_cellSaveStarted = true;
    m_closingCell = false;
    positionTableCell();
    viewport()->update();
}

void MarkdownEditor::closeTableCell(bool save) {
    if (m_closingCell || m_cellEdit->isHidden()) return;
    if (save) saveTableCell();
    m_closingCell = true;
    m_cellEdit->hide();
    if (save && !m_cellAnchor.isNull()) {
        const int line = m_cellAnchor.blockNumber() + m_cellRow + (m_cellRow > 0 ? 1 : 0);
        const QTextBlock block = document()->findBlockByNumber(line);
        if (block.isValid()) setTextCursor(QTextCursor(block));
    }
    m_closingCell = false;
    viewport()->update();
}

bool MarkdownEditor::eventFilter(QObject *object, QEvent *event) {
    if (object == m_cellEdit && event->type() == QEvent::ContextMenu) {
        auto *context = static_cast<QContextMenuEvent *>(event);
        QMenu *menu = m_cellEdit->createStandardContextMenu();
        QMenu *table = menu->addMenu(T("表格"));
        const QList<QPair<QString, const char *>> actions{
            {"row-after", "在下方插入行"}, {"row-delete", "删除当前行"},
            {"column-after", "在右侧插入列"}, {"column-delete", "删除当前列"},
            {"left", "左对齐"}, {"center", "居中对齐"}, {"right", "右对齐"}
        };
        for (const auto &[id, label] : actions) table->addAction(T(label), this, [this, id] { tableAction(id); });
        menu->exec(context->globalPos());
        delete menu;
        return true;
    }
    if (object == m_cellEdit && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape) { closeTableCell(true); setFocus(); return true; }
        if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab || key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            int row = m_cellRow, col = m_cellColumn;
            const bool backward = key->key() == Qt::Key_Backtab || key->modifiers().testFlag(Qt::ShiftModifier);
            closeTableCell(true);
            const md::Table table = md::parseTable(tableRange(m_cellAnchor.block()).selectedText().replace(QChar::ParagraphSeparator, u'\n'));
            if (!table.valid()) { setFocus(); return true; }
            if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) row += backward ? -1 : 1;
            else { col += backward ? -1 : 1; if (col < 0) { --row; col = int(table.align.size()) - 1; } if (col >= table.align.size()) { ++row; col = 0; } }
            if (row < 0) { setFocus(); return true; }
            if (row >= table.rows.size()) { m_cellRow = int(table.rows.size()) - 1; tableAction(QStringLiteral("row-after")); }
            editTableCell(row, col);
            return true;
        }
    }
    return QTextEdit::eventFilter(object, event);
}

bool MarkdownEditor::tableAction(const QString &action) {
    if (isReadOnly()) return false;
    const bool editing = !m_cellEdit->isHidden();
    closeTableCell(true);
    QTextCursor range = tableRange(editing ? m_cellAnchor.block() : textCursor().block());
    if (range.isNull()) return false;
    md::Table table = md::parseTable(range.selectedText().replace(QChar::ParagraphSeparator, u'\n'));
    if (!table.valid()) return false;
    const int row = std::clamp(m_cellRow, 0, int(table.rows.size()) - 1);
    const int col = std::clamp(m_cellColumn, 0, int(table.align.size()) - 1);
    if (action == QLatin1String("row-after") || action == QLatin1String("row-before")) {
        table.rows.insert(action == QLatin1String("row-after") ? row + 1 : std::max(1, row), QStringList(table.align.size(), QString()));
    } else if (action == QLatin1String("row-delete")) {
        if (row == 0) return false;
        table.rows.removeAt(row);
    } else if (action == QLatin1String("column-before") || action == QLatin1String("column-after")) {
        const int at = col + (action == QLatin1String("column-after") ? 1 : 0);
        table.align.insert(at, Qt::AlignLeft);
        for (QStringList &cells : table.rows) cells.insert(at, QString());
    } else if (action == QLatin1String("column-delete")) {
        if (table.align.size() == 1) return false;
        table.align.removeAt(col);
        for (QStringList &cells : table.rows) cells.removeAt(col);
    } else if (action == QLatin1String("left") || action == QLatin1String("center") || action == QLatin1String("right")) {
        table.align[col] = action == QLatin1String("left") ? Qt::AlignLeft : action == QLatin1String("right") ? Qt::AlignRight : Qt::AlignHCenter;
    } else if (action == QLatin1String("sort-ascending") || action == QLatin1String("sort-descending")) {
        QCollator collator; collator.setNumericMode(true);
        const bool ascending = action == QLatin1String("sort-ascending");
        std::stable_sort(table.rows.begin() + 1, table.rows.end(), [&](const QStringList &a, const QStringList &b) {
            const QString left = md::plainInline(a[col]), right = md::plainInline(b[col]);
            bool aNumber, bNumber;
            const double av = left.toDouble(&aNumber), bv = right.toDouble(&bNumber);
            aNumber = aNumber && std::isfinite(av);
            bNumber = bNumber && std::isfinite(bv);
            // Keep numeric and text values in separate groups so mixed
            // columns still have a strict, transitive ordering.
            if (aNumber != bNumber) return ascending ? aNumber : bNumber;
            if (aNumber) return ascending ? av < bv : av > bv;
            return ascending ? collator.compare(left, right) < 0 : collator.compare(left, right) > 0;
        });
    } else if (action != QLatin1String("format")) return false;
    const QString formatted = md::formatTable(table);
    if (formatted == range.selectedText().replace(QChar::ParagraphSeparator, u'\n')) return true;
    const int start = range.selectionStart();
    range.beginEditBlock();
    range.insertText(formatted);
    range.endEditBlock();
    setTextCursor(QTextCursor(document()->findBlock(start)));
    return true;
}

void MarkdownEditor::paintTableRow(QPainter &p, const QTextBlock &block, const QRectF &r) const {
    const Tokens &k = Theme::t();
    const auto *info = static_cast<BlockInfo *>(block.userData());
    const MarkdownHighlighter::TableGeometry g = m_hl->tableGeometry(block);
    if (g.cols == 0 || g.x.size() < 2 || info->tableRow == 1) return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    const qreal left = std::round(r.left());
    const qreal top = std::round(r.top());
    const qreal bottom = std::round(r.bottom());
    const qreal right = left + g.x.last();
    // As on the web: rules between rows, a quiet header, no grid; long
    // cells wrap and the row grows.
    p.setPen(QPen(Theme::dark() ? k.line.lighter(125) : k.line, 1));
    p.drawLine(QPointF(left, bottom), QPointF(right, bottom));
    const QStringList cells = md::tableCells(block.text());
    p.setFont(Theme::uiFont(m_px - 2, info->tableRow == 0 ? QFont::Medium : QFont::Normal));
    p.setPen(info->tableRow == 0 ? k.muted : k.ink);
    for (int c = 0; c < g.cols && c < cells.size(); ++c) {
        const QRectF cell(left + g.x[c] + 12, top + 8, g.x[c + 1] - g.x[c] - 24, bottom - top - 8);
        p.drawText(cell, Qt::AlignTop | Qt::TextWordWrap | g.align.value(c, Qt::AlignLeft), md::plainInline(cells[c]));
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

const TaskMarkerSpan *MarkdownEditor::taskMarkerAt(const QPoint &pos, QTextBlock *blockOut) const {
    const QTextCursor c = cursorForPosition(pos);
    const QTextBlock b = c.block();
    const auto *info = static_cast<BlockInfo *>(b.userData());
    if (!info || info->taskMarkers.isEmpty()) return nullptr;
    const int p = c.positionInBlock();
    for (const TaskMarkerSpan &m : info->taskMarkers) {
        if (m.kind == TaskMarkerSpan::Done) continue;
        if (p >= m.start && p <= m.end) {
            const QRectF cr = charRect(b, std::clamp(p > m.start ? p - 1 : m.start, m.start, std::max(m.start, m.end - 1)));
            if (std::abs(cr.center().y() - pos.y()) > cr.height()) return nullptr;
            *blockOut = b;
            return &m;
        }
    }
    return nullptr;
}

int MarkdownEditor::taskAt(const QPoint &pos, QTextBlock *blockOut) const {
    const QTextCursor c = cursorForPosition(pos);
    const QTextBlock b = c.block();
    const auto *info = static_cast<BlockInfo *>(b.userData());
    if (!info || info->active || m_hl->mode() != MarkdownHighlighter::Live) return -1;
    for (qsizetype t = 0; t < info->tasks.size(); ++t) {
        const int p = info->tasks[t].first;
        const QRectF cr = charRect(b, info->taskBoxes.value(t, p));
        const QRectF end = charRect(b, p + 2);
        const QRectF hit(cr.left() - 4, cr.top(), std::max<qreal>(26, end.right() - cr.left() + 8), cr.height());
        if (hit.contains(pos)) {
            *blockOut = b;
            return p;
        }
    }
    return -1;
}

void MarkdownEditor::toggleTaskAtBlock(const QTextBlock &block) {
    if (isReadOnly() || !block.isValid()) return;
    const QString before = toPlainText();
    const QString after = tasks::toggleTaskLine(before, block.blockNumber());
    if (after == before) return;
    qsizetype prefix = 0;
    const qsizetype max = std::min(before.size(), after.size());
    while (prefix < max && before[prefix] == after[prefix]) ++prefix;
    qsizetype suffix = 0;
    while (suffix < max - prefix && before[before.size() - 1 - suffix] == after[after.size() - 1 - suffix]) ++suffix;
    QTextCursor c(document());
    c.beginEditBlock();
    c.setPosition(int(prefix));
    c.setPosition(int(before.size() - suffix), QTextCursor::KeepAnchor);
    c.insertText(after.mid(prefix, after.size() - suffix - prefix));
    c.endEditBlock();
}

void MarkdownEditor::openDuePicker() {
    if (isReadOnly()) return;
    openDuePickerForBlock(textCursor().block());
}

void MarkdownEditor::openDuePickerForBlock(const QTextBlock &block) {
    if (isReadOnly() || !block.isValid()) return;
    hideInlinePopup(false);
    const QString line = block.text();
    const auto parsed = tasks::parseTaskLine(line, block.blockNumber());
    TaskDueDialog dlg(parsed ? parsed->due : std::nullopt,
                      parsed ? parsed->repeat : std::nullopt,
                      this);
    if (dlg.exec() != QDialog::Accepted) return;
    QString updated;
    if (dlg.action() == TaskDueDialog::Removed) {
        updated = tasks::setTaskDue(line, std::nullopt, QString());
    } else if (dlg.action() == TaskDueDialog::Applied) {
        const QString asTask = tasks::isTaskLine(line) ? line : tasks::lineAsTask(line);
        updated = tasks::setTaskDue(asTask, dlg.selectedDue(), dlg.selectedRepeat());
    } else {
        return;
    }
    if (updated == line) return;
    QTextCursor c(block);
    c.beginEditBlock();
    c.movePosition(QTextCursor::StartOfBlock);
    c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    c.insertText(updated);
    c.endEditBlock();
    setTextCursor(c);
    setFocus();
}

void MarkdownEditor::mousePressEvent(QMouseEvent *e) {
    if (m_popup->isVisible()) hideInlinePopup(false);
    if (e->button() == Qt::LeftButton && !isReadOnly() && mode() == MarkdownHighlighter::Live) {
        int row, column;
        QTextBlock header;
        if (tableCellAt(e->pos(), &row, &column, &header)) {
            closeTableCell(true);
            setTextCursor(QTextCursor(header));
            if ((e->modifiers() & Qt::ShiftModifier) && !m_cellAnchor.isNull() && m_cellAnchor.block() == header) {
                const auto range = tableRange(header);
                const auto table = md::parseTable(range.selectedText().replace(QChar::ParagraphSeparator, u'\n'));
                if (!table.valid()) return;
                long double sum = 0;
                int count = 0;
                for (int r = std::max(0, std::min(row, m_cellRow)); r <= std::min(int(table.rows.size()) - 1, std::max(row, m_cellRow)); ++r) {
                    for (int c = std::max(0, std::min(column, m_cellColumn)); c <= std::min(int(table.rows[r].size()) - 1, std::max(column, m_cellColumn)); ++c) {
                        bool ok = false;
                        const double value = table.rows[r][c].trimmed().toDouble(&ok);
                        if (ok && std::isfinite(value)) { sum += value; ++count; }
                    }
                }
                if (count) {
                    const QString total = QLocale().toString(double(sum), 'g', 15);
                    const QString mean = QLocale().toString(double(sum / count), 'g', 15);
                    QMenu menu(this);
                    menu.addAction(T("复制求和结果（%1）").arg(total), this, [total] { QApplication::clipboard()->setText(total); });
                    menu.addAction(T("复制平均值（%1）").arg(mean), this, [mean] { QApplication::clipboard()->setText(mean); });
                    menu.exec(e->globalPosition().toPoint());
                }
                return;
            }
            editTableCell(row, column);
            return;
        }
    }
    if (e->button() == Qt::LeftButton && !isReadOnly()) {
        QTextBlock b;
        const int task = taskAt(e->pos(), &b);
        if (task >= 0) {
            toggleTaskAtBlock(b);
            return;
        }
        if (taskMarkerAt(e->pos(), &b)) {
            const auto *info = static_cast<BlockInfo *>(b.userData());
            const bool rendered = m_hl->mode() == MarkdownHighlighter::Live && info && !info->active;
            if (rendered || (e->modifiers() & Qt::ControlModifier)) {
                openDuePickerForBlock(b);
                return;
            }
        }
    }
    if (e->button() == Qt::LeftButton) {
        const QTextBlock block = cursorForPosition(e->pos()).block();
        const auto *rendered = static_cast<BlockInfo *>(block.userData());
        if (rendered && !rendered->active && rendered->imageDocument) {
            const QRectF rect = document()->documentLayout()->blockBoundingRect(block);
            const QPointF point = QPointF(e->pos()) + QPointF(horizontalScrollBar()->value(), verticalScrollBar()->value()) - rect.topLeft() - QPointF(0, 6);
            const QString target = rendered->imageDocument->documentLayout()->anchorAt(point);
            if (!target.isEmpty()) {
                if (target.startsWith(QLatin1String("ivy-wiki:"))) emit linkActivated(QUrl::fromPercentEncoding(target.mid(9).toUtf8()), true);
                else emit linkActivated(target, false);
                return;
            }
        }
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
    if (!hand && !isReadOnly() && taskMarkerAt(e->pos(), &b)) {
        const auto *info = static_cast<BlockInfo *>(b.userData());
        hand = (m_hl->mode() == MarkdownHighlighter::Live && info && !info->active) || (e->modifiers() & Qt::ControlModifier);
    }
    if (!hand) {
        if (linkAt(e->pos(), &b)) {
            const auto *info = static_cast<BlockInfo *>(b.userData());
            hand = (m_hl->mode() == MarkdownHighlighter::Live && info && !info->active) || (e->modifiers() & Qt::ControlModifier);
        }
    }
    viewport()->setCursor(hand ? Qt::PointingHandCursor : Qt::IBeamCursor);
    QTextEdit::mouseMoveEvent(e);
}

void MarkdownEditor::contextMenuEvent(QContextMenuEvent *e) {
    if (mode() == MarkdownHighlighter::Live && !isReadOnly()) {
        int row, column;
        QTextBlock header;
        if (tableCellAt(e->pos(), &row, &column, &header)) {
            closeTableCell(true);
            setTextCursor(QTextCursor(header));
            m_cellRow = row; m_cellColumn = column;
            QMenu menu(this);
            const QList<QPair<QString, const char *>> actions{
                {"row-before", "在上方插入行"}, {"row-after", "在下方插入行"}, {"row-delete", "删除当前行"},
                {"column-before", "在左侧插入列"}, {"column-after", "在右侧插入列"}, {"column-delete", "删除当前列"},
                {"left", "左对齐"}, {"center", "居中对齐"}, {"right", "右对齐"},
                {"sort-ascending", "升序排列"}, {"sort-descending", "降序排列"}, {"format", "整理表格源码"}
            };
            for (const auto &[id, label] : actions) {
                auto *action = menu.addAction(T(label));
                if (id == QLatin1String("row-delete")) action->setEnabled(row != 0);
                connect(action, &QAction::triggered, this, [this, id] { tableAction(id); });
            }
            menu.exec(e->globalPos());
            return;
        }
    }
    // The standard menu, with undo and redo routed through ours.
    QMenu *menu = createStandardContextMenu(e->pos());
    for (QAction *a : menu->actions()) {
        if (a->objectName() == QLatin1String("edit-undo")) {
            a->disconnect();
            connect(a, &QAction::triggered, this, &MarkdownEditor::undo);
        } else if (a->objectName() == QLatin1String("edit-redo")) {
            a->disconnect();
            connect(a, &QAction::triggered, this, &MarkdownEditor::redo);
        }
    }
    menu->addSeparator();
    if (!isReadOnly()) {
        const QTextBlock clickBlock = cursorForPosition(e->pos()).block();
        menu->addAction(T("设置日期与重复…"), this, [this, clickBlock] {
            openDuePickerForBlock(clickBlock.isValid() ? clickBlock : textCursor().block());
        });
    }
    auto *copyRich = menu->addAction(T("复制为富文本"), this, [this] {
        const auto selection = textCursor().selection();
        const QString html = renderMarkdown(selection.toPlainText());
        auto *mime = new QMimeData;
        mime->setHtml(html);
        mime->setText(QTextDocumentFragment::fromHtml(html).toPlainText());
        QApplication::clipboard()->setMimeData(mime);
    });
    copyRich->setEnabled(textCursor().hasSelection());
    menu->addAction(T("重新加载图片"), RemoteImages::instance(), &RemoteImages::retryFailed);
    menu->exec(e->globalPos());
    delete menu;
}

void MarkdownEditor::focusInEvent(QFocusEvent *e) {
    QTextEdit::focusInEvent(e);
    updateActive();
}

void MarkdownEditor::focusOutEvent(QFocusEvent *e) {
    QTextEdit::focusOutEvent(e);
    hideInlinePopup(false);
    // Show the line rendered once the editor is left.
    if (!m_completer->popup()->isVisible()) setActiveLines(-1, -1);
}

// --- typing ---

void MarkdownEditor::showInsertMenu() {
    if (isReadOnly()) return;
    setFocus();
    if (m_popup->isVisible() && m_popupKind == PopupKind::Slash) return;
    QTextCursor c = textCursor();
    const QString before = c.block().text().left(c.positionInBlock());
    static const QRegularExpression existingSlash(QStringLiteral(R"((?:^|[\s\x{3000}])/([^\s/]*)$)"));
    if (!existingSlash.match(before).hasMatch()) {
        const bool needSpace = !before.isEmpty() && !before.back().isSpace() && before.back() != QChar(0x3000);
        m_slashAutoStart = c.position();
        c.beginEditBlock();
        c.insertText(needSpace ? QStringLiteral(" /") : QStringLiteral("/"));
        c.endEditBlock();
        setTextCursor(c);
        m_slashAutoInserted = true;
    }
    updateInlinePopup();
}

void MarkdownEditor::insertElement(const QString &id) {
    if (isReadOnly()) return;
    setFocus();
    if (id == QLatin1String("h1")) return setHeading(1);
    if (id == QLatin1String("h2")) return setHeading(2);
    if (id == QLatin1String("h3")) return setHeading(3);
    if (id == QLatin1String("bold")) return toggleFormat(QStringLiteral("**"));
    if (id == QLatin1String("italic")) return toggleFormat(QStringLiteral("*"));
    if (id == QLatin1String("strike")) return toggleFormat(QStringLiteral("~~"));
    if (id == QLatin1String("highlight")) return toggleFormat(QStringLiteral("=="));
    if (id == QLatin1String("code")) return toggleFormat(QStringLiteral("`"));
    if (id == QLatin1String("todo")) return toggleList(QStringLiteral("- [ ]"));
    if (id == QLatin1String("bullet")) return toggleList(QStringLiteral("-"));
    if (id == QLatin1String("ordered")) return toggleList(QStringLiteral("1."));
    if (id == QLatin1String("due")) return openDuePicker();
    if (id == QLatin1String("file")) { emit attachmentRequested(); return; }
    if (id == QLatin1String("wiki")) {
        insertPlainText(QStringLiteral("[[]]"));
        QTextCursor c = textCursor();
        c.movePosition(QTextCursor::Left, QTextCursor::MoveAnchor, 2);
        setTextCursor(c);
        updateCompletion();
        return;
    }
    if (id == QLatin1String("today") || id == QLatin1String("now")) {
        insertPlainText(QDateTime::currentDateTime().toString(id == QLatin1String("today") ? QStringLiteral("yyyy-MM-dd") : QStringLiteral("yyyy-MM-dd HH:mm")));
        return;
    }
    if (id == QLatin1String("properties")) {
        const QTextBlock first = document()->firstBlock();
        if (first.text().trimmed() == QLatin1String("---")) {
            QTextCursor c(first.next().isValid() ? first.next() : first);
            c.movePosition(QTextCursor::EndOfBlock);
            setTextCursor(c);
            ensureCursorVisible();
            return;
        }
        QTextCursor c(document());
        c.beginEditBlock();
        c.movePosition(QTextCursor::Start);
        c.insertText(QStringLiteral("---\ntags: []\n---\n\n"));
        c.endEditBlock();
        c.setPosition(11); // inside []
        setTextCursor(c);
        ensureCursorVisible();
        return;
    }
    if (id == QLatin1String("mermaid")) {
        insertBlockText(QStringLiteral("```mermaid\nflowchart TD\n    A[Start] --> B[End]\n```\n"));
        QTextCursor c = textCursor();
        c.movePosition(QTextCursor::PreviousBlock);
        c.movePosition(QTextCursor::PreviousBlock);
        c.movePosition(QTextCursor::EndOfBlock);
        setTextCursor(c);
        return;
    }
    if (id == QLatin1String("link") || id == QLatin1String("image")) {
        bool ok = false;
        const QString url = QInputDialog::getText(this, id == QLatin1String("image") ? T("网络图片") : T("网页链接"), T("网址"), QLineEdit::Normal, QStringLiteral("https://"), &ok).trimmed();
        if (!ok || !isSafeExternalUrl(url) || (id == QLatin1String("image") && !RemoteImages::isRemote(url))) return;
        QTextCursor c = textCursor();
        QString label = c.selectedText().replace(QChar::ParagraphSeparator, u' ');
        if (label.isEmpty() && id == QLatin1String("link")) label = url;
        label.replace(u'\\', QStringLiteral("\\\\")).replace(u'[', QStringLiteral("\\[")).replace(u']', QStringLiteral("\\]"));
        const QString target = QUrl(url).toString(QUrl::FullyEncoded);
        c.insertText((id == QLatin1String("image") ? QStringLiteral("!") : QString()) + QStringLiteral("[%1](<%2>)").arg(label, target));
        setTextCursor(c);
        return;
    }
    if (id == QLatin1String("quote")) {
        QTextCursor c = textCursor();
        c.beginEditBlock();
        const int first = document()->findBlock(c.selectionStart()).blockNumber();
        const int last = document()->findBlock(c.hasSelection() ? c.selectionEnd() - 1 : c.position()).blockNumber();
        for (int i = first; i <= last; ++i) { QTextCursor row(document()->findBlockByNumber(i)); row.insertText(QStringLiteral("> ")); }
        c.endEditBlock();
        return;
    }
    if (id == QLatin1String("divider")) return insertBlockText(QStringLiteral("---\n"));
    if (id == QLatin1String("table")) return insertBlockText(QStringLiteral("| A | B |\n| --- | --- |\n|  |  |\n"));
    if (id == QLatin1String("codeblock")) {
        QTextCursor c = textCursor();
        const QString selected = c.selectedText().replace(QChar::ParagraphSeparator, u'\n');
        QString fence = QStringLiteral("```");
        while (selected.contains(fence)) fence += u'`';
        insertBlockText(fence + u'\n' + selected + u'\n' + fence + u'\n');
        c = textCursor();
        c.movePosition(QTextCursor::PreviousBlock);
        c.movePosition(QTextCursor::PreviousBlock);
        c.movePosition(QTextCursor::EndOfBlock);
        setTextCursor(c);
    }
}

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
    const int end = c.hasSelection() ? c.selectionEnd() - 1 : c.selectionEnd();
    const int b = document()->findBlock(end).blockNumber();
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
    const int end = c.hasSelection() ? c.selectionEnd() - 1 : c.selectionEnd();
    const int b = document()->findBlock(end).blockNumber();
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
    const auto *info = static_cast<BlockInfo *>(c.block().userData());
    if (info && (info->kind == BlockInfo::Code || info->kind == BlockInfo::Fence)) return false;
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
    if (e->matches(QKeySequence::Undo)) return undo();
    if (e->matches(QKeySequence::Redo)) return redo();
    if (m_popup->isVisible()) {
        switch (e->key()) {
        case Qt::Key_Down:
            moveInlinePopupSelection(1);
            return;
        case Qt::Key_Up:
            moveInlinePopupSelection(-1);
            return;
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Tab:
            acceptInlinePopupItem(m_popupList->currentItem());
            return;
        case Qt::Key_Escape:
            hideInlinePopup(true);
            return;
        default:
            break;
        }
    }
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
    if (!isReadOnly() && mode() == MarkdownHighlighter::Live && e->key() == Qt::Key_Tab && !textCursor().hasSelection()) {
        const auto *info = static_cast<BlockInfo *>(textCursor().block().userData());
        if (info && info->kind == BlockInfo::Table) {
            editTableCell(info->tableRow > 1 ? info->tableRow - 1 : 0, 0);
            return;
        }
    }
    const auto mods = e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier);
    const bool ctrl = mods & Qt::ControlModifier;
    if (!isReadOnly() && ctrl) {
        if (e->key() == Qt::Key_Slash) return showInsertMenu();
        const bool shift = mods & Qt::ShiftModifier;
        const bool alt = mods & Qt::AltModifier;
        if (!shift && !alt) {
            if (e->key() == Qt::Key_B) return toggleFormat(QStringLiteral("**"));
            if (e->key() == Qt::Key_I) return toggleFormat(QStringLiteral("*"));
            if (e->key() == Qt::Key_E) return toggleFormat(QStringLiteral("`"));
        }
        if (shift && !alt) {
            if (e->key() == Qt::Key_D) { openDuePicker(); return; }
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
            const int end = c.hasSelection() ? c.selectionEnd() - 1 : c.selectionEnd();
            const int b = document()->findBlock(end).blockNumber();
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
    if (!e->text().isEmpty() || e->key() == Qt::Key_Backspace) {
        updateCompletion();
        updateInlinePopup();
    }
}

void MarkdownEditor::inputMethodEvent(QInputMethodEvent *e) {
    QTextEdit::inputMethodEvent(e);
    if (!e->commitString().isEmpty()) {
        updateCompletion();
        updateInlinePopup();
    }
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

bool MarkdownEditor::inlinePopupVisible() const {
    return m_popup && m_popup->isVisible();
}

QStringList MarkdownEditor::inlinePopupItemIds() const {
    QStringList ids;
    if (!m_popupList) return ids;
    for (int i = 0; i < m_popupList->count(); ++i) {
        const auto *it = m_popupList->item(i);
        if (!it || it->data(RoleIsHeader).toBool()) continue;
        ids << it->data(RoleId).toString();
    }
    return ids;
}

void MarkdownEditor::hideInlinePopup(bool revertAutoSlash) {
    const bool revert = revertAutoSlash && m_slashAutoInserted && m_popupKind == PopupKind::Slash && m_popupFrom >= 0;
    const int from = (m_slashAutoStart >= 0 && m_slashAutoStart <= m_popupFrom) ? m_slashAutoStart : m_popupFrom;
    const int to = m_popupTo;
    m_popupKind = PopupKind::None;
    m_popupFrom = -1;
    m_popupTo = -1;
    m_slashAutoInserted = false;
    m_slashAutoStart = -1;
    if (m_popup) m_popup->hide();
    if (revert) {
        const int maxPos = std::max(0, document()->characterCount() - 1);
        const int a = std::clamp(from, 0, maxPos);
        const int b = std::clamp(to, a, maxPos);
        if (b > a) {
            QTextCursor c(document());
            c.beginEditBlock();
            c.setPosition(a);
            c.setPosition(b, QTextCursor::KeepAnchor);
            c.removeSelectedText();
            c.endEditBlock();
            setTextCursor(c);
        }
    }
}

void MarkdownEditor::positionInlinePopup() {
    if (!m_popup || !m_popupList || m_popupList->count() == 0) return;
    int totalH = 8;
    for (int i = 0; i < m_popupList->count(); ++i) {
        const auto *it = m_popupList->item(i);
        totalH += it && it->data(RoleIsHeader).toBool() ? 24 : 34;
    }
    const int popupW = std::min(320, std::max(240, width() - 24));
    const int popupH = std::min(300, totalH + 4);
    const QRect cr = cursorRect();
    const QPoint vpPos = viewport()->mapTo(this, cr.bottomLeft());
    const QPoint vpTop = viewport()->mapTo(this, cr.topLeft());
    int x = std::clamp(vpPos.x(), 8, std::max(8, width() - popupW - 8));
    int y = vpPos.y() + 6;
    if (y + popupH > height() - 8 && vpTop.y() - popupH - 6 >= 8) {
        y = vpTop.y() - popupH - 6;
    }
    m_popup->setGeometry(x, y, popupW, popupH);
    m_popup->raise();
}

void MarkdownEditor::moveInlinePopupSelection(int delta) {
    if (!m_popupList || m_popupList->count() == 0) return;
    const int n = m_popupList->count();
    int cur = m_popupList->currentRow();
    for (int step = 0; step < n; ++step) {
        cur = (cur + delta + n) % n;
        const auto *it = m_popupList->item(cur);
        if (it && !it->data(RoleIsHeader).toBool()) {
            m_popupList->setCurrentRow(cur);
            m_popupList->scrollToItem(it);
            return;
        }
    }
}

void MarkdownEditor::updateInlinePopup() {
    if (isReadOnly() || m_loading || m_completer->popup()->isVisible()) {
        hideInlinePopup(false);
        return;
    }
    const QTextCursor c = textCursor();
    if (c.hasSelection()) {
        hideInlinePopup(false);
        return;
    }
    const QTextBlock block = c.block();
    const QString line = block.text();
    const QString before = line.left(c.positionInBlock());
    const auto *info = static_cast<BlockInfo *>(block.userData());
    const bool suppressed = (info && (info->kind == BlockInfo::Code || info->kind == BlockInfo::Fence)) ||
                            inInlineCode(before) || inFrontmatter(block);
    if (suppressed && !m_slashAutoInserted) {
        hideInlinePopup(false);
        return;
    }

    const bool zh = i18n::isZh();

    // 1. Natural-language @ date completion on task lines.
    if (tasks::isTaskLine(line)) {
        static const QRegularExpression atRe(QStringLiteral(R"((?:^|[\s\x{3000}])@([^@\n]{0,24})$)"));
        if (const auto m = atRe.match(before); m.hasMatch()) {
            const qsizetype atIdx = before.lastIndexOf(u'@');
            const QString query = m.captured(1);
            const QDateTime now = QDateTime::currentDateTime();
            const QDate today = now.date();
            m_popupList->clear();
            auto addDateOption = [&](const QString &id, const QString &label, const QString &hint, const tasks::Due &due) {
                auto *it = new QListWidgetItem(label, m_popupList);
                it->setData(RoleId, id);
                it->setData(RoleIcon, QStringLiteral("📅"));
                it->setData(RoleHint, hint);
                it->setData(RoleIsHeader, false);
                it->setData(RoleDueDate, due.date);
                it->setData(RoleDueTime, due.time);
            };
            if (query.trimmed().isEmpty()) {
                for (const auto &q : tasks::quickDates(now, zh)) {
                    addDateOption(QStringLiteral("date:") + q.due.dateString(), q.label, q.due.dateString(), q.due);
                }
            } else if (const auto parsed = tasks::parseWhen(query, now)) {
                const QString fmt = parsed->allDay() ? parsed->dateString() : parsed->dateString() + u' ' + parsed->timeString();
                addDateOption(QStringLiteral("date:") + fmt, tasks::describeDue(*parsed, today, zh), fmt, *parsed);
                if (parsed->allDay()) {
                    tasks::Due withMorning = *parsed;
                    withMorning.time = QTime(9, 0);
                    const QString mFmt = withMorning.dateString() + QStringLiteral(" 09:00");
                    addDateOption(QStringLiteral("date:") + mFmt, tasks::describeDue(withMorning, today, zh), mFmt, withMorning);
                }
            }
            auto *pickerItem = new QListWidgetItem(T("选择日期和重复…"), m_popupList);
            pickerItem->setData(RoleId, QStringLiteral("__picker__"));
            pickerItem->setData(RoleIcon, QStringLiteral("🔁"));
            pickerItem->setData(RoleHint, QStringLiteral("Ctrl+Shift+D"));
            pickerItem->setData(RoleIsHeader, false);

            m_popupKind = PopupKind::Date;
            m_popupFrom = block.position() + int(atIdx);
            m_popupTo = c.position();
            m_slashAutoInserted = false;
            m_popupList->setCurrentRow(0);
            positionInlinePopup();
            m_popup->show();
            return;
        }
    }

    // 2. Slash command menu (/query).
    static const QRegularExpression slashRe(QStringLiteral(R"((?:^|[\s\x{3000}])/([^\s/]*)$)"));
    if (const auto m = slashRe.match(before); m.hasMatch()) {
        const qsizetype slashIdx = before.lastIndexOf(u'/');
        const QString query = m.captured(1);
        const QList<insertitems::Item> items = insertitems::match(query);
        if (items.isEmpty()) {
            hideInlinePopup(false);
            return;
        }
        m_popupList->clear();
        QString lastSection;
        int firstSelectable = -1;
        for (const auto &item : items) {
            if (query.isEmpty() && item.section != lastSection) {
                lastSection = item.section;
                auto *hdr = new QListWidgetItem(insertitems::sectionLabel(item.section, zh), m_popupList);
                hdr->setData(RoleIsHeader, true);
                hdr->setFlags(Qt::NoItemFlags);
            }
            auto *it = new QListWidgetItem(zh ? item.zh : item.en, m_popupList);
            it->setData(RoleId, item.id);
            it->setData(RoleIcon, item.icon);
            it->setData(RoleHint, item.hint);
            it->setData(RoleDesc, zh ? item.descZh : item.descEn);
            it->setData(RoleIsHeader, false);
            if (firstSelectable < 0) firstSelectable = m_popupList->count() - 1;
        }
        m_popupKind = PopupKind::Slash;
        m_popupFrom = block.position() + int(slashIdx);
        m_popupTo = c.position();
        if (firstSelectable >= 0) m_popupList->setCurrentRow(firstSelectable);
        positionInlinePopup();
        m_popup->show();
        return;
    }

    hideInlinePopup(false);
}

void MarkdownEditor::acceptInlinePopupItem(QListWidgetItem *item) {
    if (!item || item->data(RoleIsHeader).toBool()) return;
    const PopupKind kind = m_popupKind;
    const int from = m_popupFrom;
    const int to = m_popupTo;
    const QString id = item->data(RoleId).toString();
    const QDate dueDate = item->data(RoleDueDate).toDate();
    const QTime dueTime = item->data(RoleDueTime).toTime();
    hideInlinePopup(false);

    const int maxPos = std::max(0, document()->characterCount() - 1);
    const int a = std::clamp(from, 0, maxPos);
    const int b = std::clamp(to, a, maxPos);

    if (kind == PopupKind::Slash) {
        if (b > a) {
            QTextCursor c(document());
            c.beginEditBlock();
            c.setPosition(a);
            c.setPosition(b, QTextCursor::KeepAnchor);
            c.removeSelectedText();
            c.endEditBlock();
            setTextCursor(c);
        }
        insertElement(id);
        return;
    }

    if (kind == PopupKind::Date) {
        QTextCursor c(document());
        c.beginEditBlock();
        if (b > a) {
            c.setPosition(a);
            c.setPosition(b, QTextCursor::KeepAnchor);
            c.removeSelectedText();
        }
        if (id == QLatin1String("__picker__")) {
            c.endEditBlock();
            setTextCursor(c);
            openDuePicker();
            return;
        }
        const QTextBlock blk = c.block();
        QString line = blk.text();
        // Clean trailing whitespace left behind by removing "@query".
        while (line.endsWith(u' ')) line.chop(1);
        const tasks::Due due{dueDate, dueTime};
        const QString updated = tasks::setTaskDue(line, due, std::nullopt);
        c.setPosition(blk.position());
        c.setPosition(blk.position() + int(blk.text().size()), QTextCursor::KeepAnchor);
        c.insertText(updated);
        c.endEditBlock();
        setTextCursor(c);
        setFocus();
    }
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

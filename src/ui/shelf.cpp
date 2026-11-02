#include "ui/shelf.h"

#include "core/format.h"
#include "core/i18n.h"
#include "core/models.h"
#include "sync/engine.h"
#include "ui/icons.h"
#include "ui/theme.h"
#include "ui/toast.h"
#include "ui/widgets.h"
#include "vault/session.h"

#include <QApplication>
#include <QDateTime>
#include <QDrag>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QScrollBar>
#include <QSettings>
#include <QToolButton>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

const char *kNoteMime = "application/x-ivynote-note";
const char *kFolderMime = "application/x-ivynote-folder";
constexpr int kRowH = 30;
constexpr int kGap = 10;

QString settingsKey(Session *s, const char *k) { return QStringLiteral("vault/%1/%2").arg(s->info().id, QLatin1String(k)); }

// The vault name with a small chevron, as one quiet button.
class VaultButton : public QToolButton {
public:
    explicit VaultButton(QWidget *parent) : QToolButton(parent) {
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        setFont(Theme::uiFont(14.5, QFont::DemiBold));
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        setFixedHeight(32);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const Tokens &k = Theme::t();
        const QFontMetrics fm(font());
        const int chevron = 14;
        const int textW = std::min(fm.horizontalAdvance(text()), width() - 16 - chevron - 4);
        const QRectF bg(0, 0, std::min(width(), textW + 16 + chevron + 4), height());
        if (underMouse() || isDown() || hasFocus()) {
            p.setPen(Qt::NoPen);
            p.setBrush(Theme::alpha(k.ink, isDown() ? 0.09 : 0.055));
            p.drawRoundedRect(bg.adjusted(0.5, 2.5, -0.5, -2.5), Theme::radius, Theme::radius);
        }
        p.setFont(font());
        p.setPen(k.ink);
        p.drawText(QRect(8, 0, textW, height()), Qt::AlignVCenter | Qt::AlignLeft, fm.elidedText(text(), Qt::ElideRight, textW));
        p.drawPixmap(QRect(8 + textW + 4, (height() - chevron) / 2 + 1, chevron, chevron),
                     icons::pixmap(QStringLiteral("chevron-down"), underMouse() ? k.ink2 : k.muted, chevron, devicePixelRatioF()));
    }
};

QStringList splitIds(const QString &ids) { return ids.split(u'\n', Qt::SkipEmptyParts); }

} // namespace

// --- model ---

int ShelfModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : int(m_rows.size()); }

QVariant ShelfModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_rows.size()) return {};
    const ShelfRow &r = m_rows[index.row()];
    if (role == Qt::DisplayRole || role == Qt::AccessibleTextRole) return r.untitled ? T("无标题") : (r.kind == TreeRow::Group || r.kind == TreeRow::Heading || r.kind == TreeRow::Create ? T(r.label) : r.label);
    if (role == RowRole) return index.row();
    return {};
}

Qt::ItemFlags ShelfModel::flags(const QModelIndex &index) const {
    if (!index.isValid()) return Qt::ItemIsDropEnabled;
    const ShelfRow &r = m_rows[index.row()];
    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if ((r.kind == TreeRow::NoteRow && !r.trash) || r.kind == TreeRow::FolderRow) f |= Qt::ItemIsDragEnabled;
    if (r.dropFolder != kNoDrop) f |= Qt::ItemIsDropEnabled;
    return f;
}

QStringList ShelfModel::mimeTypes() const { return {QLatin1String(kNoteMime), QLatin1String(kFolderMime)}; }

QMimeData *ShelfModel::mimeData(const QModelIndexList &indexes) const {
    if (indexes.isEmpty()) return nullptr;
    const ShelfRow &r = m_rows[indexes.first().row()];
    auto *m = new QMimeData;
    m->setData(r.kind == TreeRow::FolderRow ? kFolderMime : kNoteMime, r.id.toUtf8());
    return m;
}

void ShelfModel::setRows(const QList<ShelfRow> &rows) {
    bool sameShape = rows.size() == m_rows.size();
    for (int i = 0; sameShape && i < rows.size(); ++i) sameShape = rows[i].key == m_rows[i].key && rows[i].gap == m_rows[i].gap;
    if (sameShape) {
        m_rows = rows;
        if (!m_rows.isEmpty()) emit dataChanged(index(0), index(int(m_rows.size()) - 1));
        return;
    }
    beginResetModel();
    m_rows = rows;
    endResetModel();
}

int ShelfModel::indexOfKey(const QString &key) const {
    for (int i = 0; i < m_rows.size(); ++i)
        if (m_rows[i].key == key) return i;
    return -1;
}

int ShelfModel::indexOfNote(const QString &noteId) const {
    // Prefer where the note lives over the pinned and tag lenses.
    int fallback = -1;
    for (int i = 0; i < m_rows.size(); ++i) {
        const ShelfRow &r = m_rows[i];
        if (r.kind != TreeRow::NoteRow || r.id != noteId) continue;
        if (!r.parent.startsWith(QLatin1String("v:pinned")) && !r.parent.startsWith(QLatin1String("t:"))) return i;
        if (fallback < 0) fallback = i;
    }
    return fallback;
}

// --- delegate ---

QSize ShelfDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const {
    const auto *m = static_cast<const ShelfModel *>(index.model());
    const ShelfRow &r = m->row(index.row());
    return {100, kRowH + (r.gap ? kGap : 0) + (r.kind == TreeRow::Heading ? 4 : 0)};
}

QRect ShelfDelegate::headingButton(const QRect &row, int i) {
    const int s = 24;
    return {row.right() - 8 - (3 - i) * (s + 2), row.bottom() - kRowH + (kRowH - s) / 2 + 1, s, s};
}

bool ShelfDelegate::isSelected(const ShelfRow &r) const {
    if (r.kind != TreeRow::NoteRow) return false;
    if (!multi.isEmpty()) return multi.contains(r.id);
    return !selectedNote.isEmpty() && r.id == selectedNote;
}

void ShelfDelegate::paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const {
    const auto *m = static_cast<const ShelfModel *>(index.model());
    const ShelfRow &r = m->row(index.row());
    const Tokens &k = Theme::t();
    const qreal dpr = p->device()->devicePixelRatioF();
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    if (motion < 1.0 && fresh.contains(r.key)) {
        // New rows drop in from just above.
        p->setOpacity(motion);
        p->translate(0, -(1.0 - motion) * 6);
    }
    QRect rect = opt.rect;
    rect.setTop(rect.bottom() - kRowH + 1);
    const QRect pill = rect.adjusted(6, 1, -6, -1);
    const qreal rad = Theme::radius;
    const bool hover = index.row() == hoverRow;
    const bool selected = isSelected(r);
    const bool isDrop = !dropKey.isEmpty() && r.key == dropKey;

    if (isDrop) {
        p->setPen(QPen(Theme::alpha(k.ivy, 0.5), 1));
        p->setBrush(k.ivySoft);
        p->drawRoundedRect(QRectF(pill).adjusted(0.5, 0.5, -0.5, -0.5), rad, rad);
    } else if (selected) {
        p->setPen(Qt::NoPen);
        p->setBrush(multi.isEmpty() ? Theme::alpha(k.ink, 0.085) : Theme::alpha(k.ivy, Theme::dark() ? 0.28 : 0.16));
        p->drawRoundedRect(pill, rad, rad);
    } else if (hover && r.kind != TreeRow::Heading) {
        p->setPen(Qt::NoPen);
        p->setBrush(Theme::alpha(k.ink, 0.045));
        p->drawRoundedRect(pill, rad, rad);
    }
    if (opt.state & QStyle::State_HasFocus && opt.state & QStyle::State_Selected && !selected) {
        p->setPen(QPen(Theme::alpha(k.ivy, 0.5), 1));
        p->setBrush(Qt::NoBrush);
        p->drawRoundedRect(QRectF(pill).adjusted(0.5, 0.5, -0.5, -0.5), rad, rad);
    }

    if (r.kind == TreeRow::Heading) {
        p->setPen(k.muted);
        p->setFont(Theme::uiFont(12, QFont::Medium));
        p->drawText(rect.adjusted(14, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, T(r.label));
        if (hover) {
            const char *names[] = {"sort", "folder", "plus"};
            for (int i = 0; i < 3; ++i) {
                const QRect b = headingButton(opt.rect, i);
                p->drawPixmap(b.adjusted(4, 4, -4, -4), icons::pixmap(QLatin1String(names[i]), k.muted, 16, dpr));
            }
        }
        p->restore();
        return;
    }

    const int indent = 8 + r.depth * 16 + 4;
    // Ancestor guide lines.
    p->setPen(QPen(k.line, 1));
    for (int d = 0; d < r.depth; ++d) {
        const qreal x = 8 + d * 16 + 4 + 8 + 0.5;
        p->drawLine(QPointF(x, opt.rect.top()), QPointF(x, opt.rect.bottom() + 1));
    }
    int x = indent;
    const int cy = rect.center().y();
    // Disclosure triangle, turned 90° when open.
    if (r.expandable) {
        p->save();
        p->translate(x + 8, cy + 0.5);
        if (motion < 1.0 && r.key == turnKey) p->rotate(90 * (turnOpen ? motion : 1.0 - motion));
        else if (r.open) p->rotate(90);
        p->drawPixmap(QRectF(-7, -7, 14, 14), icons::pixmap(QStringLiteral("disclosure"), hover ? k.ink2 : k.faint, 14, dpr), QRectF());
        p->restore();
    }
    x += 18;
    if (r.kind != TreeRow::More) {
        QColor ic = k.muted;
        QString icon = r.icon;
        if (r.kind == TreeRow::FolderRow) {
            ic = k.ivy;
            icon = QStringLiteral("folder-fill");
        } else if (r.kind == TreeRow::TagRow) {
            ic = Theme::tagColor(r.label, r.tagColor);
        } else if (r.kind == TreeRow::Create) {
            ic = k.faint;
        }
        p->drawPixmap(QRect(x, cy - 8, 16, 16), icons::pixmap(icon, ic, 16, dpr));
        x += 24;
    }
    // Right side: pending dot, pin, count.
    int right = rect.right() - 14;
    p->setFont(Theme::uiFont(12));
    if (r.count > 0 && r.kind != TreeRow::NoteRow && r.kind != TreeRow::More) {
        const QString c = QString::number(r.count);
        const int w = p->fontMetrics().horizontalAdvance(c);
        p->setPen(k.faint);
        p->drawText(QRect(right - w, rect.top(), w, rect.height()), Qt::AlignVCenter | Qt::AlignRight, c);
        right -= w + 8;
    }
    if (r.kind == TreeRow::NoteRow && r.pinned && !r.parent.startsWith(QLatin1String("v:pinned"))) {
        p->drawPixmap(QRect(right - 13, cy - 6, 13, 13), icons::pixmap(QStringLiteral("pin"), k.faint, 13, dpr));
        right -= 19;
    }
    if (r.pending) {
        p->setPen(Qt::NoPen);
        p->setBrush(k.warn);
        p->drawEllipse(QPointF(right - 3, cy + 0.5), 3, 3);
        right -= 12;
    }
    QString label;
    QColor color = k.ink2;
    QFont font = Theme::uiFont(13.5);
    switch (r.kind) {
    case TreeRow::Group: label = T(r.label); color = k.ink2; font.setWeight(QFont::Medium); break;
    case TreeRow::More: label = (i18n::isZh() ? QStringLiteral("再显示 %1 篇") : QStringLiteral("Show %1 more")).arg(r.count); color = k.muted; break;
    case TreeRow::Create: label = T(r.label); color = k.muted; break;
    case TreeRow::NoteRow: label = r.untitled ? T("无标题") : r.label; color = r.untitled ? k.muted : (selected ? k.ink : k.ink2); break;
    default: label = r.label; color = k.ink2; break;
    }
    if (r.trash && r.kind == TreeRow::NoteRow) color = k.muted;
    p->setFont(font);
    p->setPen(color);
    const QRect textRect(x, rect.top(), right - x, rect.height());
    p->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, p->fontMetrics().elidedText(label, Qt::ElideRight, textRect.width()));
    p->restore();
}

// --- view ---

ShelfView::ShelfView(QWidget *parent) : QListView(parent), m_delegate(new ShelfDelegate(this)) {
    setItemDelegate(m_delegate);
    setFrameShape(QFrame::NoFrame);
    setMouseTracking(true);
    setSelectionMode(QAbstractItemView::SingleSelection); // multi selection is kept by hand
    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(false);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::MoveAction);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setUniformItemSizes(false);
    setAttribute(Qt::WA_MacShowFocusRect, false);
    viewport()->setAutoFillBackground(false);
    m_expandTimer.setSingleShot(true);
    m_expandTimer.setInterval(650);
    connect(&m_expandTimer, &QTimer::timeout, this, [this] {
        if (m_expandRow >= 0 && m_expandRow < model()->rowCount() && !shelfModel()->row(m_expandRow).open) emit toggle(m_expandRow);
    });
}

ShelfModel *ShelfView::shelfModel() const { return static_cast<ShelfModel *>(model()); }

QStringList ShelfView::selectedNotes() const {
    QStringList ids;
    for (const ShelfRow &r : shelfModel()->rows())
        if (r.kind == TreeRow::NoteRow && m_delegate->multi.contains(r.id) && !ids.contains(r.id)) ids << r.id;
    return ids;
}

void ShelfView::clearMulti() {
    if (m_delegate->multi.isEmpty()) return;
    m_delegate->multi.clear();
    viewport()->update();
}

void ShelfView::toggleMulti(const ShelfRow &r) {
    QSet<QString> &multi = m_delegate->multi;
    // Trash and live notes are not picked together: they take different actions.
    if (!multi.isEmpty()) {
        const int row = shelfModel()->indexOfNote(*multi.begin());
        if (row >= 0 && shelfModel()->row(row).trash != r.trash) multi.clear();
    }
    if (multi.isEmpty() && !m_delegate->selectedNote.isEmpty() && m_delegate->selectedNote != r.id) {
        const int open = shelfModel()->indexOfNote(m_delegate->selectedNote);
        if (open >= 0 && shelfModel()->row(open).trash == r.trash) multi.insert(m_delegate->selectedNote);
    }
    if (multi.contains(r.id)) multi.remove(r.id);
    else multi.insert(r.id);
    viewport()->update();
}

void ShelfView::selectRange(int from, int to) {
    if (from > to) std::swap(from, to);
    const bool trash = shelfModel()->row(to).trash;
    m_delegate->multi.clear();
    for (int i = from; i <= to; ++i) {
        const ShelfRow &r = shelfModel()->row(i);
        if (r.kind == TreeRow::NoteRow && r.trash == trash) m_delegate->multi.insert(r.id);
    }
    viewport()->update();
}

void ShelfView::mousePressEvent(QMouseEvent *e) {
    const QModelIndex idx = indexAt(e->pos());
    m_releaseOpens = -1;
    if (e->button() == Qt::LeftButton && idx.isValid()) {
        const ShelfRow &r = shelfModel()->row(idx.row());
        if (r.kind == TreeRow::Heading) {
            for (int i = 0; i < 3; ++i)
                if (ShelfDelegate::headingButton(visualRect(idx), i).contains(e->pos())) {
                    emit headingButton(i, viewport()->mapToGlobal(e->pos()));
                    return;
                }
            return;
        }
        setCurrentIndex(idx);
        if (r.kind == TreeRow::NoteRow) {
            const auto mods = e->modifiers();
            QListView::mousePressEvent(e); // allows dragging
            if (mods & (Qt::ControlModifier | Qt::MetaModifier)) {
                toggleMulti(r);
                m_anchorKey = r.key;
                return;
            }
            if (mods & Qt::ShiftModifier) {
                int anchor = shelfModel()->indexOfKey(m_anchorKey);
                if (anchor < 0) anchor = shelfModel()->indexOfNote(m_delegate->selectedNote);
                selectRange(anchor < 0 ? idx.row() : anchor, idx.row());
                return;
            }
            m_anchorKey = r.key;
            if (m_delegate->multi.size() > 1 && m_delegate->multi.contains(r.id)) {
                // Maybe the start of dragging the whole selection.
                m_releaseOpens = idx.row();
                return;
            }
            clearMulti();
            emit activated(idx.row(), false);
            return;
        }
        clearMulti();
        if (r.kind == TreeRow::FolderRow) {
            QListView::mousePressEvent(e);
        }
        emit activated(idx.row(), false);
        return;
    }
    QListView::mousePressEvent(e);
}

void ShelfView::mouseReleaseEvent(QMouseEvent *e) {
    QListView::mouseReleaseEvent(e);
    if (m_releaseOpens >= 0 && m_releaseOpens < model()->rowCount() && indexAt(e->pos()).row() == m_releaseOpens) {
        const int row = m_releaseOpens;
        m_releaseOpens = -1;
        clearMulti();
        emit activated(row, false);
    }
    m_releaseOpens = -1;
}

void ShelfView::mouseMoveEvent(QMouseEvent *e) {
    const int row = indexAt(e->pos()).row();
    if (row != m_delegate->hoverRow) {
        m_delegate->hoverRow = row;
        viewport()->update();
    }
    QListView::mouseMoveEvent(e);
}

void ShelfView::leaveEvent(QEvent *e) {
    m_delegate->hoverRow = -1;
    viewport()->update();
    QListView::leaveEvent(e);
}

void ShelfView::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape && !m_delegate->multi.isEmpty()) return clearMulti();
    if (e->matches(QKeySequence::SelectAll)) {
        // Every live note that is showing.
        m_delegate->multi.clear();
        for (const ShelfRow &r : shelfModel()->rows())
            if (r.kind == TreeRow::NoteRow && !r.trash) m_delegate->multi.insert(r.id);
        viewport()->update();
        return;
    }
    const QModelIndex idx = currentIndex();
    if (!idx.isValid()) return QListView::keyPressEvent(e);
    const ShelfRow &r = shelfModel()->row(idx.row());
    if (e->key() == Qt::Key_Up || e->key() == Qt::Key_Down) clearMulti();
    switch (e->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        emit activated(idx.row(), true);
        return;
    case Qt::Key_Right:
        if (r.expandable && !r.open) emit toggle(idx.row());
        else if (idx.row() + 1 < model()->rowCount()) setCurrentIndex(model()->index(idx.row() + 1, 0));
        return;
    case Qt::Key_Left:
        if (r.expandable && r.open) {
            emit toggle(idx.row());
        } else if (!r.parent.isEmpty()) {
            const int p = shelfModel()->indexOfKey(r.parent);
            if (p >= 0) setCurrentIndex(model()->index(p, 0));
        }
        return;
    case Qt::Key_Delete:
        emit deletePressed(idx.row());
        return;
    case Qt::Key_F2:
        emit renamePressed(idx.row());
        return;
    default:
        break;
    }
    QListView::keyPressEvent(e);
    // Arrow keys walk the rows and open notes as they go.
    if ((e->key() == Qt::Key_Up || e->key() == Qt::Key_Down) && currentIndex().isValid() &&
        shelfModel()->row(currentIndex().row()).kind == TreeRow::NoteRow)
        emit activated(currentIndex().row(), true);
}

void ShelfView::contextMenuEvent(QContextMenuEvent *e) {
    const QModelIndex idx = indexAt(e->pos());
    if (idx.isValid()) emit contextMenu(idx.row(), e->globalPos());
}

int ShelfView::dropRowAt(const QPoint &pos, const QMimeData *mime) const {
    const QModelIndex idx = indexAt(pos);
    if (!idx.isValid()) return -1;
    const ShelfRow &r = shelfModel()->row(idx.row());
    if (r.dropFolder == kNoDrop) return -1;
    if (mime->hasFormat(kFolderMime) && r.kind == TreeRow::NoteRow && !r.dropFolder.isEmpty()) {
        // Dropping a notebook on a note files it into that note's notebook.
        return shelfModel()->indexOfKey(r.dropFolder);
    }
    if (r.kind == TreeRow::NoteRow) return shelfModel()->indexOfKey(r.dropFolder.isEmpty() ? QStringLiteral("h:notes") : r.dropFolder);
    return idx.row();
}

void ShelfView::dragEnterEvent(QDragEnterEvent *e) {
    if (e->mimeData()->hasFormat(kNoteMime) || e->mimeData()->hasFormat(kFolderMime)) e->acceptProposedAction();
    else e->ignore();
}

void ShelfView::dragMoveEvent(QDragMoveEvent *e) {
    const int row = dropRowAt(e->position().toPoint(), e->mimeData());
    const QString key = row >= 0 ? shelfModel()->row(row).key : QString();
    if (key != m_delegate->dropKey) {
        m_delegate->dropKey = key;
        viewport()->update();
    }
    const int under = indexAt(e->position().toPoint()).row();
    if (under != m_expandRow) {
        m_expandRow = under;
        m_expandTimer.start();
    }
    if (row >= 0) e->acceptProposedAction();
    else e->ignore();
    // Scroll near the edges.
    const int y = e->position().toPoint().y();
    if (y < 24) verticalScrollBar()->setValue(verticalScrollBar()->value() - 8);
    else if (y > viewport()->height() - 24) verticalScrollBar()->setValue(verticalScrollBar()->value() + 8);
}

void ShelfView::dragLeaveEvent(QDragLeaveEvent *e) {
    m_delegate->dropKey.clear();
    m_expandTimer.stop();
    viewport()->update();
    QListView::dragLeaveEvent(e);
}

void ShelfView::dropEvent(QDropEvent *e) {
    m_expandTimer.stop();
    m_delegate->dropKey.clear();
    viewport()->update();
    const int row = dropRowAt(e->position().toPoint(), e->mimeData());
    if (row < 0) return e->ignore();
    const QString mime = e->mimeData()->hasFormat(kFolderMime) ? QLatin1String(kFolderMime) : QLatin1String(kNoteMime);
    clearMulti();
    emit dropped(mime, QString::fromUtf8(e->mimeData()->data(mime)), row);
    e->setDropAction(Qt::CopyAction); // the model is rebuilt; nothing to remove
    e->accept();
}

void ShelfView::startDrag(Qt::DropActions actions) {
    m_releaseOpens = -1;
    const QModelIndex idx = currentIndex();
    if (!idx.isValid()) return;
    QMimeData *mime = model()->mimeData({idx});
    if (!mime) return;
    const ShelfRow &r = shelfModel()->row(idx.row());
    QString text = idx.data().toString();
    int count = 1;
    if (r.kind == TreeRow::NoteRow && m_delegate->multi.size() > 1 && m_delegate->multi.contains(r.id)) {
        // The whole selection travels together.
        const QStringList ids = selectedNotes();
        mime->setData(kNoteMime, ids.join(u'\n').toUtf8());
        count = int(ids.size());
        text = (i18n::isZh() ? QStringLiteral("%1 篇笔记") : QStringLiteral("%1 notes")).arg(count);
    }
    auto *drag = new QDrag(this);
    drag->setMimeData(mime);
    // A small label with the title (or the count) as the drag image.
    const Tokens &k = Theme::t();
    QFont f = Theme::uiFont(13);
    const int w = std::min(240, QFontMetrics(f).horizontalAdvance(text) + 28);
    const int stack = count > 1 ? 3 : 0;
    QPixmap pm(QSize(w + stack, 28 + stack) * devicePixelRatioF());
    pm.setDevicePixelRatio(devicePixelRatioF());
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(k.line.darker(Theme::dark() ? 70 : 115), 1));
    if (stack) {
        p.setBrush(k.sunken);
        p.drawRoundedRect(QRectF(stack + 0.5, stack + 0.5, w - 1, 27), Theme::radius, Theme::radius);
    }
    p.setBrush(k.surface);
    p.drawRoundedRect(QRectF(0.5, 0.5, w - 1, 27), Theme::radius, Theme::radius);
    p.setFont(f);
    p.setPen(k.ink);
    p.drawText(QRect(14, 0, w - 28, 28), Qt::AlignVCenter, QFontMetrics(f).elidedText(text, Qt::ElideRight, w - 28));
    p.end();
    drag->setPixmap(pm);
    drag->setHotSpot(QPoint(14, 14));
    drag->exec(actions & Qt::MoveAction ? Qt::MoveAction : actions);
}

// --- shelf ---

Shelf::Shelf(QWidget *parent) : QWidget(parent), m_model(new ShelfModel(this)), m_view(new ShelfView(this)) {
    setMinimumWidth(220);
    auto *col = new QVBoxLayout(this);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);

    auto *header = new QWidget(this);
    header->setFixedHeight(48);
    auto *hrow = new QHBoxLayout(header);
    hrow->setContentsMargins(6, 8, 8, 0);
    hrow->setSpacing(2);
    m_vaultButton = new VaultButton(header);
    m_vaultButton->setToolTip(T("切换笔记库"));
    connect(m_vaultButton, &QToolButton::clicked, this, [this] {
        emit vaultMenuRequested(m_vaultButton->mapToGlobal(QPoint(0, m_vaultButton->height() + 2)));
    });
    hrow->addWidget(m_vaultButton, 1);
    m_searchButton = new IconButton(QStringLiteral("search"), T("搜索") + QStringLiteral(" (Ctrl+K)"), header);
    m_newButton = new IconButton(QStringLiteral("edit"), T("新建笔记") + QStringLiteral(" (Ctrl+N)"), header);
    m_collapseButton = new IconButton(QStringLiteral("sidebar"), T("收起书架") + QStringLiteral(" (Ctrl+\\)"), header);
    connect(m_searchButton, &QToolButton::clicked, this, &Shelf::searchRequested);
    connect(m_newButton, &QToolButton::clicked, this, [this] { emit newNote(QString()); });
    connect(m_collapseButton, &QToolButton::clicked, this, &Shelf::collapseRequested);
    hrow->addWidget(m_searchButton);
    hrow->addWidget(m_newButton);
    hrow->addWidget(m_collapseButton);
    col->addWidget(header);

    m_view->setModel(m_model);
    col->addWidget(m_view, 1);

    auto *footer = new QWidget(this);
    footer->setFixedHeight(44);
    auto *frow = new QHBoxLayout(footer);
    frow->setContentsMargins(12, 4, 8, 6);
    frow->setSpacing(6);
    m_statusDot = new QLabel(footer);
    m_statusDot->setFixedSize(10, 10);
    frow->addWidget(m_statusDot);
    m_status = new QPushButton(footer);
    m_status->setFlat(true);
    m_status->setCursor(Qt::PointingHandCursor);
    m_status->setFont(Theme::uiFont(12));
    m_status->setStyleSheet(QStringLiteral("QPushButton{border:none;padding:2px 2px;text-align:left;background:transparent;}"));
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    connect(m_status, &QPushButton::clicked, this, &Shelf::syncStatusClicked);
    frow->addWidget(m_status, 1);
    m_settingsButton = new IconButton(QStringLiteral("settings"), T("设置") + QStringLiteral(" (Ctrl+,)"), footer);
    connect(m_settingsButton, &QToolButton::clicked, this, &Shelf::settingsRequested);
    frow->addWidget(m_settingsButton);
    col->addWidget(footer);

    m_rebuildTimer.setSingleShot(true);
    m_rebuildTimer.setInterval(0);
    connect(&m_rebuildTimer, &QTimer::timeout, this, &Shelf::rebuild);
    m_motion.setStartValue(0.0);
    m_motion.setEndValue(1.0);
    m_motion.setDuration(200);
    m_motion.setEasingCurve(Theme::easeOut());
    connect(&m_motion, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        m_view->delegate()->motion = v.toReal();
        m_view->viewport()->update();
    });
    connect(&m_motion, &QVariantAnimation::finished, this, [this] {
        m_view->delegate()->motion = 1.0;
        m_view->delegate()->fresh.clear();
        m_view->viewport()->update();
    });
    m_clock.setInterval(30000);
    connect(&m_clock, &QTimer::timeout, this, &Shelf::updateStatus);
    m_clock.start();

    connect(m_view, &ShelfView::activated, this, &Shelf::onActivated);
    connect(m_view, &ShelfView::toggle, this, &Shelf::onToggle);
    connect(m_view, &ShelfView::contextMenu, this, &Shelf::onContextMenu);
    connect(m_view, &ShelfView::headingButton, this, &Shelf::onHeadingButton);
    connect(m_view, &ShelfView::dropped, this, &Shelf::onDropped);
    connect(m_view, &ShelfView::deletePressed, this, &Shelf::deleteRow);
    connect(m_view, &ShelfView::renamePressed, this, &Shelf::renameRow);
    connect(Theme::instance(), &Theme::changed, this, [this] {
        updateStatus();
        m_view->viewport()->update();
    });
}

void Shelf::setSession(Session *session) {
    if (m_session) saveState();
    if (m_engine) m_engine->disconnect(this);
    m_session = session;
    m_engine = session ? session->engine() : nullptr;
    m_current.clear();
    m_view->delegate()->selectedNote.clear();
    m_view->delegate()->multi.clear();
    if (session) {
        loadState();
        connect(m_engine, &SyncEngine::dataChanged, this, [this] { m_rebuildTimer.start(); });
        connect(m_engine, &SyncEngine::stateChanged, this, &Shelf::updateStatus);
        connect(session, &Session::userChanged, this, &Shelf::updateStatus);
        connect(session, &Session::needsSignIn, this, &Shelf::updateStatus);
    }
    updateHeader();
    rebuild();
    updateStatus();
}

void Shelf::updateHeader() {
    const QString name = m_session ? m_session->info().name : QString();
    m_vaultButton->setText(name);
    m_vaultButton->update();
    m_vaultButton->setToolTip(m_session && m_session->isRemote()
                                  ? QStringLiteral("%1\n%2 · %3").arg(name, m_session->info().server, m_session->info().username)
                                  : name + u'\n' + T("本地笔记库"));
}

void Shelf::loadState() {
    QSettings s;
    m_expanded.clear();
    const QStringList list = s.value(settingsKey(m_session, "expanded"), QStringList{QStringLiteral("v:pinned")}).toStringList();
    for (const QString &k : list) m_expanded.insert(k);
    m_sort = SortBy(s.value(settingsKey(m_session, "sort"), 0).toInt());
    m_limits.clear();
}

void Shelf::saveState() {
    if (!m_session) return;
    QSettings s;
    s.setValue(settingsKey(m_session, "expanded"), QStringList(m_expanded.begin(), m_expanded.end()));
    s.setValue(settingsKey(m_session, "sort"), int(m_sort));
}

void Shelf::rebuild() {
    QList<ShelfRow> rows;
    if (m_engine) {
        TreeInput in;
        in.folders = m_engine->folderList();
        in.notes = m_engine->noteList();
        in.tags = m_engine->tagList();
        in.expanded = m_expanded;
        in.sortBy = m_sort;
        in.limits = m_limits;
        for (const TreeRow &t : buildTree(in)) {
            ShelfRow r;
            r.key = t.key;
            r.kind = t.kind;
            r.depth = t.depth;
            r.parent = t.parent;
            r.label = t.label;
            r.icon = t.icon;
            r.count = t.count;
            r.expandable = t.expandable;
            r.open = t.open;
            r.gap = t.gap;
            r.trash = t.trash;
            r.dropFolder = t.dropFolder;
            if (t.note) {
                r.id = t.note->id;
                r.pinned = t.note->pinned;
                r.untitled = t.note->title.trimmed().isEmpty();
                r.pending = m_engine->hasPending(EntityType::Note, t.note->id);
            } else if (t.folder) {
                r.id = t.folder->id;
                r.pending = m_engine->hasPending(EntityType::Folder, t.folder->id);
            } else if (t.tag) {
                r.id = t.tag->id;
                r.tagColor = t.tag->color;
            }
            rows << r;
        }
    }
    // Rows that were not there before fade in (an opened notebook, a new note).
    QSet<QString> before;
    for (const ShelfRow &r : m_model->rows()) before.insert(r.key);
    QSet<QString> fresh;
    QSet<QString> notes;
    for (const ShelfRow &r : std::as_const(rows)) {
        if (!before.isEmpty() && !before.contains(r.key)) fresh.insert(r.key);
        if (r.kind == TreeRow::NoteRow) notes.insert(r.id);
    }
    // A picked note that is no longer showing is no longer picked.
    m_view->delegate()->multi.intersect(notes);
    if (!fresh.isEmpty() && !Theme::reducedMotion() && isVisible()) {
        m_view->delegate()->fresh.unite(fresh);
        m_view->delegate()->motion = 0.0;
        m_motion.stop();
        m_motion.start();
    }
    const int scroll = m_view->verticalScrollBar()->value();
    m_model->setRows(rows);
    m_view->verticalScrollBar()->setValue(scroll);
    const int cur = m_model->indexOfNote(m_current);
    if (cur >= 0 && m_view->currentIndex().row() != cur) {
        const QSignalBlocker block(m_view->selectionModel());
        m_view->setCurrentIndex(m_model->index(cur, 0));
    }
}

void Shelf::setCurrentNote(const QString &noteId) {
    m_current = noteId;
    m_view->delegate()->selectedNote = noteId;
    if (m_engine && !noteId.isEmpty()) {
        // Open the path down to the note.
        if (const Note *n = m_engine->note(noteId)) {
            bool changed = false;
            if (n->trashed()) {
                changed = !m_expanded.contains(QStringLiteral("v:trash"));
                m_expanded.insert(QStringLiteral("v:trash"));
            } else {
                for (const Folder *f : m_engine->folderPath(n->folderId)) {
                    if (!m_expanded.contains(f->id)) changed = true;
                    m_expanded.insert(f->id);
                }
            }
            if (changed) rebuild();
        }
    }
    const int row = m_model->indexOfNote(noteId);
    if (row >= 0) {
        const QSignalBlocker block(m_view->selectionModel());
        m_view->setCurrentIndex(m_model->index(row, 0));
        m_view->scrollTo(m_model->index(row, 0));
    }
    m_view->viewport()->update();
}

void Shelf::focusTree() { m_view->setFocus(); }

void Shelf::onActivated(int row, bool keyboard) {
    const ShelfRow r = m_model->row(row);
    switch (r.kind) {
    case TreeRow::NoteRow:
        emit openNote(r.id);
        break;
    case TreeRow::More:
        m_limits[r.parent] = m_limits.value(r.parent, kTreePage) + kTreePage;
        rebuild();
        break;
    case TreeRow::Create:
        newFolder(QString());
        break;
    case TreeRow::Group:
    case TreeRow::FolderRow:
    case TreeRow::TagRow:
        if (!keyboard || r.expandable) onToggle(row);
        break;
    default:
        break;
    }
}

void Shelf::onToggle(int row) {
    const ShelfRow &r = m_model->row(row);
    if (!r.expandable) return;
    if (m_expanded.contains(r.key)) m_expanded.remove(r.key);
    else m_expanded.insert(r.key);
    m_view->delegate()->turnKey = r.key;
    m_view->delegate()->turnOpen = m_expanded.contains(r.key);
    if (!Theme::reducedMotion()) {
        m_view->delegate()->motion = 0.0;
        m_motion.stop();
        m_motion.start();
    }
    rebuild();
    saveState();
}

void Shelf::newFolder(const QString &parentId) {
    if (!m_engine) return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, T("新建笔记本"), T("名称"), QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    const Folder f = m_engine->createFolder(name.left(100), parentId);
    if (!parentId.isEmpty()) m_expanded.insert(parentId);
    rebuild();
    const int idx = m_model->indexOfKey(f.id);
    if (idx >= 0) m_view->scrollTo(m_model->index(idx, 0));
}

void Shelf::renameRow(int row) {
    if (!m_engine) return;
    const ShelfRow r = m_model->row(row);
    bool ok = false;
    if (r.kind == TreeRow::FolderRow) {
        const QString name = QInputDialog::getText(this, T("重命名笔记本"), T("名称"), QLineEdit::Normal, r.label, &ok).trimmed();
        if (ok && !name.isEmpty()) m_engine->updateFolder(r.id, name.left(100), std::nullopt);
    } else if (r.kind == TreeRow::TagRow) {
        const QString name = QInputDialog::getText(this, T("重命名标签"), T("名称"), QLineEdit::Normal, r.label, &ok).trimmed();
        if (ok && !name.isEmpty()) m_engine->updateTag(r.id, name.left(50), std::nullopt);
    } else if (r.kind == TreeRow::NoteRow && !r.trash) {
        const QString name = QInputDialog::getText(this, T("重命名"), T("标题"), QLineEdit::Normal, r.untitled ? QString() : r.label, &ok);
        if (ok) {
            NotePatch p;
            p.title = name.simplified().left(300);
            m_engine->updateNote(r.id, p);
        }
    }
}

void Shelf::trashNotes(const QStringList &ids, bool trashed) {
    if (!m_engine || ids.isEmpty()) return;
    QStringList changed;
    for (const QString &id : ids) {
        const Note *n = m_engine->note(id);
        if (!n || n->trashed() == trashed) continue;
        NotePatch p;
        p.trashed = trashed;
        m_engine->updateNote(id, p);
        changed << id;
    }
    m_view->clearMulti();
    if (!trashed || changed.isEmpty()) return;
    QPointer<SyncEngine> e(m_engine);
    const QString text = changed.size() == 1 ? T("已移到回收站")
                                             : (i18n::isZh() ? QStringLiteral("已将 %1 篇笔记移到回收站") : QStringLiteral("Moved %1 notes to the trash")).arg(changed.size());
    Toaster::show(text, false, T("撤销"), [e, changed] {
        if (!e) return;
        for (const QString &id : changed) {
            NotePatch u;
            u.trashed = false;
            e->updateNote(id, u);
        }
    });
}

void Shelf::deleteRow(int row) {
    if (!m_engine) return;
    const ShelfRow r = m_model->row(row);
    const QStringList picked = m_view->selectedNotes();
    if (r.kind == TreeRow::NoteRow && picked.size() > 1 && picked.contains(r.id)) {
        if (!r.trash) return trashNotes(picked, true);
        const QString text = (i18n::isZh() ? QStringLiteral("永久删除这 %1 篇笔记？此操作无法撤销。") : QStringLiteral("Permanently delete these %1 notes? This cannot be undone.")).arg(picked.size());
        if (QMessageBox::question(this, T("永久删除"), text) != QMessageBox::Yes) return;
        m_view->clearMulti();
        for (const QString &id : picked) m_engine->deleteNote(id);
        return;
    }
    if (r.kind == TreeRow::NoteRow) {
        if (r.trash) {
            if (QMessageBox::question(this, T("永久删除"), T("永久删除这篇笔记？此操作无法撤销。")) == QMessageBox::Yes)
                m_engine->deleteNote(r.id);
            return;
        }
        trashNotes({r.id}, true);
    } else if (r.kind == TreeRow::FolderRow) {
        const auto answer = QMessageBox::question(
            this, T("删除笔记本"),
            (i18n::isZh() ? QStringLiteral("删除「%1」？其中的子笔记本会一起删除，笔记移到回收站。") : QStringLiteral("Delete “%1”? Its sub-notebooks are deleted too and its notes move to the trash.")).arg(r.label));
        if (answer == QMessageBox::Yes) m_engine->deleteFolder(r.id);
    } else if (r.kind == TreeRow::TagRow) {
        const auto answer = QMessageBox::question(
            this, T("删除标签"), (i18n::isZh() ? QStringLiteral("删除标签 #%1？笔记本身不受影响。") : QStringLiteral("Delete tag #%1? Notes keep their content.")).arg(r.label));
        if (answer == QMessageBox::Yes) m_engine->deleteTag(r.id);
    }
}

void Shelf::onContextMenu(int row, const QPoint &global) {
    if (!m_engine) return;
    const ShelfRow r = m_model->row(row);
    const QStringList picked = m_view->selectedNotes();
    if (r.kind == TreeRow::NoteRow && picked.size() > 1 && picked.contains(r.id)) return batchMenu(picked, r.trash, global);
    m_view->clearMulti();
    QMenu menu(this);
    QPointer<SyncEngine> e(m_engine);
    const QString id = r.id;
    if (r.kind == TreeRow::FolderRow) {
        menu.addAction(icons::themed("plus"), T("新建笔记"), this, [this, id] { emit newNote(id); });
        menu.addAction(icons::themed("folder"), T("新建子笔记本"), this, [this, id] { newFolder(id); });
        menu.addAction(icons::themed("edit"), T("重命名"), this, [this, row] { renameRow(row); });
        menu.addSeparator();
        menu.addAction(icons::themed("trash"), T("删除"), this, [this, row] { deleteRow(row); });
    } else if (r.kind == TreeRow::NoteRow && r.trash) {
        menu.addAction(icons::themed("restore"), T("恢复"), this, [e, id] {
            NotePatch p;
            p.trashed = false;
            if (e) e->updateNote(id, p);
        });
        menu.addAction(icons::themed("trash"), T("永久删除"), this, [this, row] { deleteRow(row); });
    } else if (r.kind == TreeRow::NoteRow) {
        menu.addAction(icons::themed("pin"), r.pinned ? T("取消置顶") : T("设为置顶"), this, [e, id, pinned = r.pinned] {
            NotePatch p;
            p.pinned = !pinned;
            if (e) e->updateNote(id, p);
        });
        menu.addAction(icons::themed("edit"), T("重命名"), this, [this, row] { renameRow(row); });
        menu.addAction(icons::themed("move"), T("移动到…"), this, [this, id] { emit moveNotes({id}); });
        menu.addSeparator();
        menu.addAction(icons::themed("trash"), T("移到回收站"), this, [this, row] { deleteRow(row); });
    } else if (r.kind == TreeRow::TagRow) {
        menu.addAction(icons::themed("edit"), T("重命名"), this, [this, row] { renameRow(row); });
        auto *colors = menu.addMenu(T("颜色"));
        const QList<QPair<QString, const char *>> list{{"", "自动"}, {"green", "绿色"}, {"blue", "蓝色"}, {"amber", "琥珀"},
                                                       {"pink", "粉色"}, {"purple", "紫色"}, {"gray", "灰色"}};
        for (const auto &[c, label] : list) {
            QPixmap dot(12, 12);
            dot.fill(Qt::transparent);
            QPainter p(&dot);
            p.setRenderHint(QPainter::Antialiasing);
            p.setBrush(Theme::tagColor(r.label, c));
            p.setPen(Qt::NoPen);
            p.drawEllipse(1, 1, 10, 10);
            p.end();
            QAction *a = colors->addAction(QIcon(dot), T(label), this, [e, id, c] {
                if (e) e->updateTag(id, std::nullopt, c);
            });
            a->setCheckable(true);
            a->setChecked(r.tagColor == c);
        }
        menu.addSeparator();
        menu.addAction(icons::themed("trash"), T("删除"), this, [this, row] { deleteRow(row); });
    } else if (r.key == QLatin1String("v:trash")) {
        QAction *a = menu.addAction(icons::themed("trash"), T("清空回收站"), this, &Shelf::emptyTrashRequested);
        a->setEnabled(r.count > 0);
    } else {
        return;
    }
    menu.exec(global);
}

void Shelf::batchMenu(const QStringList &ids, bool trash, const QPoint &global) {
    QMenu menu(this);
    QPointer<SyncEngine> e(m_engine);
    auto *head = menu.addAction((i18n::isZh() ? QStringLiteral("已选 %1 篇笔记") : QStringLiteral("%1 notes selected")).arg(ids.size()));
    head->setEnabled(false);
    menu.addSeparator();
    if (trash) {
        menu.addAction(icons::themed("restore"), T("恢复"), this, [this, ids] { trashNotes(ids, false); });
        menu.addAction(icons::themed("trash"), T("永久删除"), this, [this, ids] {
            const int row = m_model->indexOfNote(ids.first());
            if (row >= 0) deleteRow(row);
        });
    } else {
        bool allPinned = true;
        for (const QString &id : ids)
            if (const Note *n = m_engine->note(id); n && !n->pinned) allPinned = false;
        menu.addAction(icons::themed("pin"), allPinned ? T("取消置顶") : T("设为置顶"), this, [this, e, ids, allPinned] {
            for (const QString &id : ids) {
                NotePatch p;
                p.pinned = !allPinned;
                if (e) e->updateNote(id, p);
            }
            m_view->clearMulti();
        });
        menu.addAction(icons::themed("move"), T("移动到…"), this, [this, ids] {
            emit moveNotes(ids);
            m_view->clearMulti();
        });
        menu.addSeparator();
        menu.addAction(icons::themed("trash"), T("移到回收站"), this, [this, ids] { trashNotes(ids, true); });
    }
    menu.addSeparator();
    menu.addAction(T("取消选择"), this, [this] { m_view->clearMulti(); });
    menu.exec(global);
}

void Shelf::onHeadingButton(int which, const QPoint &global) {
    if (which == 0) {
        QMenu menu(this);
        const QList<QPair<SortBy, const char *>> opts{{SortBy::Updated, "更新时间"}, {SortBy::Created, "创建时间"}, {SortBy::Title, "标题"}};
        for (const auto &[s, label] : opts) {
            QAction *a = menu.addAction(T(label), this, [this, s] {
                m_sort = s;
                saveState();
                rebuild();
            });
            a->setCheckable(true);
            a->setChecked(m_sort == s);
        }
        menu.exec(global);
    } else if (which == 1) {
        newFolder(QString());
    } else {
        emit newNote(QString());
    }
}

void Shelf::onDropped(const QString &mime, const QString &ids, int row) {
    if (!m_engine) return;
    const ShelfRow r = m_model->row(row);
    const QString target = r.kind == TreeRow::Heading ? QString() : r.dropFolder;
    if (mime == QLatin1String(kFolderMime)) {
        const QString id = ids;
        if (id == target) return;
        if (!m_engine->canMoveFolder(id, target)) {
            Toaster::show(T("不能把笔记本移到它自己里面"), true);
            return;
        }
        m_engine->updateFolder(id, std::nullopt, target);
    } else {
        int moved = 0;
        for (const QString &id : splitIds(ids)) {
            const Note *n = m_engine->note(id);
            if (!n || n->trashed() || n->folderId == target) continue;
            NotePatch p;
            p.folderId = target;
            m_engine->updateNote(id, p);
            moved++;
        }
        if (moved > 1)
            Toaster::show((i18n::isZh() ? QStringLiteral("已移动 %1 篇笔记") : QStringLiteral("Moved %1 notes")).arg(moved));
    }
    if (!target.isEmpty()) m_expanded.insert(target);
    rebuild();
}

void Shelf::updateStatus() {
    const Tokens &k = Theme::t();
    QColor dot;
    QString text;
    bool pulse = false;
    if (!m_session || !m_engine) {
        text.clear();
    } else if (!m_session->isRemote()) {
        text = T("本地笔记库 · 仅保存在本机");
    } else {
        const SyncState &s = m_engine->state();
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (!m_session->signedIn() || m_engine->paused()) {
            text = T("需要重新登录 · 更改保存在本机");
            dot = k.danger;
        } else if (s.blocked > 0) {
            text = (i18n::isZh() ? QStringLiteral("%1 条更改被服务器拒绝") : QStringLiteral("%1 changes refused by the server")).arg(s.blocked);
            dot = k.danger;
        } else if (s.status == SyncState::Syncing) {
            text = T("同步中…");
        } else if (s.status == SyncState::Offline) {
            text = s.pending + s.uploads > 0
                       ? (i18n::isZh() ? QStringLiteral("离线中 · %1 条更改将在联网后自动同步") : QStringLiteral("Offline · %1 changes will sync when online")).arg(s.pending + s.uploads)
                       : T("离线中");
            dot = k.warn;
        } else if (s.status == SyncState::Error) {
            text = T("同步出错");
            dot = k.danger;
        } else if (s.pending + s.uploads > 0) {
            text = (i18n::isZh() ? QStringLiteral("%1 条更改待同步") : QStringLiteral("%1 changes waiting")).arg(s.pending + s.uploads);
            dot = k.warn;
            pulse = true;
        } else {
            text = T("已同步") + QStringLiteral(" · ") + fmt::ago(s.lastSyncAt, now);
            dot = k.ok;
        }
        if (s.status == SyncState::Error && !s.error.isEmpty() && s.error != QLatin1String("unauthorized"))
            m_status->setToolTip(s.error);
        else
            m_status->setToolTip(T("点击立即同步"));
    }
    Q_UNUSED(pulse);
    QPixmap pm(QSize(10, 10) * devicePixelRatioF());
    pm.setDevicePixelRatio(devicePixelRatioF());
    pm.fill(Qt::transparent);
    if (dot.isValid()) {
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(dot);
        p.drawEllipse(QRectF(2, 2, 6, 6));
    }
    m_statusDot->setPixmap(pm);
    m_statusDot->setVisible(dot.isValid());
    QPalette pal = m_status->palette();
    pal.setColor(QPalette::ButtonText, k.muted);
    m_status->setPalette(pal);
    m_status->setText(m_status->fontMetrics().elidedText(text, Qt::ElideRight, std::max(60, m_status->width() - 4)));
    m_status->setProperty("fullText", text);
    if (!text.isEmpty() && m_status->toolTip().isEmpty()) m_status->setToolTip(text);
}

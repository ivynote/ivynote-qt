#include "ui/palette.h"

#include "core/format.h"
#include "core/i18n.h"
#include "core/models.h"
#include "core/search.h"
#include "core/tree.h"
#include "sync/engine.h"
#include "ui/icons.h"
#include "ui/theme.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QDateTime>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QScreen>
#include <QStyledItemDelegate>
#include <QTextDocument>
#include <QVBoxLayout>

namespace {

enum Roles { KindRole = Qt::UserRole + 1, IdRole, TitleRole, SnippetRole, MetaRole, MarksRole, IndexRole };

// Two lines per hit: title, then the snippet with matches highlighted.
class HitDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const override {
        return {100, index.data(SnippetRole).toString().isEmpty() ? 38 : 56};
    }
    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const override {
        const Tokens &k = Theme::t();
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRect r = opt.rect.adjusted(6, 2, -6, -2);
        if (opt.state & QStyle::State_Selected) {
            p->setPen(Qt::NoPen);
            p->setBrush(Theme::alpha(k.ink, 0.07));
            p->drawRoundedRect(r, Theme::radius, Theme::radius);
        }
        const QString icon = index.data(Qt::DecorationRole).toString();
        p->drawPixmap(QRect(r.left() + 10, r.top() + 10, 16, 16), icons::pixmap(icon, k.muted, 16, p->device()->devicePixelRatioF()));
        const int x = r.left() + 36;
        const QString meta = index.data(MetaRole).toString();
        p->setFont(Theme::uiFont(12));
        const int metaW = meta.isEmpty() ? 0 : p->fontMetrics().horizontalAdvance(meta) + 8;
        p->setPen(k.faint);
        p->drawText(QRect(r.right() - metaW - 8, r.top() + 8, metaW, 20), Qt::AlignRight | Qt::AlignVCenter, meta);
        p->setFont(Theme::uiFont(14, QFont::Medium));
        p->setPen(k.ink);
        const QRect titleRect(x, r.top() + 8, r.right() - x - metaW - 12, 20);
        p->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, p->fontMetrics().elidedText(index.data(TitleRole).toString(), Qt::ElideRight, titleRect.width()));
        const QString snippet = index.data(SnippetRole).toString();
        if (!snippet.isEmpty()) {
            const QVariantList marks = index.data(MarksRole).toList();
            QString html;
            int last = 0;
            for (const QVariant &v : marks) {
                const QPoint m = v.toPoint();
                if (m.x() < last) continue;
                html += snippet.mid(last, m.x() - last).toHtmlEscaped();
                html += QStringLiteral("<span style=\"background-color:%1;color:%2\">%3</span>")
                            .arg(Theme::alpha(k.warn, 0.25).name(QColor::HexArgb), k.ink.name(), snippet.mid(m.x(), m.y() - m.x()).toHtmlEscaped());
                last = m.y();
            }
            html += snippet.mid(last).toHtmlEscaped();
            QTextDocument doc;
            doc.setDefaultFont(Theme::uiFont(12.5));
            doc.setDefaultStyleSheet(QStringLiteral("body{color:%1;}").arg(k.muted.name()));
            doc.setHtml(QStringLiteral("<body>") + html + QStringLiteral("</body>"));
            doc.setDocumentMargin(0);
            doc.setTextWidth(r.right() - x - 8);
            p->translate(x, r.top() + 30);
            p->setClipRect(QRect(0, 0, r.right() - x - 8, 18));
            doc.drawContents(p);
        }
        p->restore();
    }
};

} // namespace

CommandPalette::CommandPalette(SyncEngine *engine, QList<PaletteCommand> commands, QWidget *parent)
    : QDialog(parent, Qt::FramelessWindowHint | Qt::Popup), m_engine(engine), m_commands(std::move(commands)) {
    // Opaque and square like a native popup menu: translucent popups turn
    // black on desktops without a compositor.
    setAttribute(Qt::WA_DeleteOnClose);
    auto *col = new QVBoxLayout(this);
    col->setContentsMargins(8, 8, 8, 8);
    col->setSpacing(8);
    m_input = new QLineEdit(this);
    m_input->setFrame(false);
    m_input->setFont(Theme::uiFont(16));
    m_input->setPlaceholderText(T("搜索笔记，输入 > 执行命令"));
    m_input->setClearButtonEnabled(true);
    m_input->setMinimumHeight(40);
    QPalette ip = m_input->palette();
    ip.setColor(QPalette::Base, Theme::t().surface);
    m_input->setPalette(ip);
    col->addWidget(m_input);
    m_list = new QListWidget(this);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setItemDelegate(new HitDelegate(m_list));
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setUniformItemSizes(false);
    QPalette lp = m_list->palette();
    lp.setColor(QPalette::Base, Theme::t().surface);
    m_list->setPalette(lp);
    col->addWidget(m_list, 1);
    connect(m_input, &QLineEdit::textChanged, this, &CommandPalette::update);
    connect(m_input, &QLineEdit::returnPressed, this, &CommandPalette::activate);
    connect(m_list, &QListWidget::itemClicked, this, [this] { activate(); });
    m_input->installEventFilter(this);
}

void CommandPalette::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), Theme::t().surface);
    p.setPen(QPen(Theme::dark() ? Theme::t().line.lighter(140) : Theme::t().line.darker(125), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5));
}

void CommandPalette::open(const QString &initial) {
    QWidget *host = parentWidget()->window();
    const int w = std::min(560 + 16, host->width() - 40);
    const int h = std::min(480, int(host->height() * 0.7));
    resize(w, h);
    move(host->mapToGlobal(QPoint((host->width() - w) / 2, int(host->height() * 0.12))));
    m_input->setText(initial);
    update(initial);
    if (!Theme::reducedMotion()) {
        // A quick fade where the window system can do it; a plain show elsewhere.
        setWindowOpacity(0.0);
        auto *a = new QPropertyAnimation(this, "windowOpacity", this);
        a->setDuration(120);
        a->setStartValue(0.0);
        a->setEndValue(1.0);
        a->setEasingCurve(Theme::easeOut());
        a->start(QAbstractAnimation::DeleteWhenStopped);
    }
    show();
    m_input->setFocus();
}

bool CommandPalette::eventFilter(QObject *o, QEvent *e) {
    if (o == m_input && e->type() == QEvent::KeyPress) {
        auto *k = static_cast<QKeyEvent *>(e);
        const int n = m_list->count();
        if (k->key() == Qt::Key_Down || k->key() == Qt::Key_Up) {
            if (n == 0) return true;
            int row = m_list->currentRow() + (k->key() == Qt::Key_Down ? 1 : -1);
            row = (row + n) % n;
            m_list->setCurrentRow(row);
            return true;
        }
        if (k->key() == Qt::Key_Escape) {
            close();
            return true;
        }
    }
    return QDialog::eventFilter(o, e);
}

void CommandPalette::update(const QString &raw) {
    m_list->clear();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (raw.startsWith(u'>')) {
        const QString q = raw.mid(1).trimmed();
        for (int i = 0; i < m_commands.size(); ++i) {
            const PaletteCommand &c = m_commands[i];
            if (!q.isEmpty() && !c.title.contains(q, Qt::CaseInsensitive)) continue;
            auto *it = new QListWidgetItem(m_list);
            it->setData(KindRole, 1);
            it->setData(IndexRole, i);
            it->setData(TitleRole, c.title);
            it->setData(MetaRole, c.shortcut);
            it->setData(Qt::DecorationRole, c.icon.isEmpty() ? QStringLiteral("zap") : c.icon);
        }
    } else if (raw.trimmed().isEmpty()) {
        // Recent notes.
        QList<const Note *> notes;
        for (const Note *n : m_engine->noteList())
            if (!n->trashed()) notes << n;
        std::sort(notes.begin(), notes.end(), [](const Note *a, const Note *b) { return a->updatedAt > b->updatedAt; });
        for (int i = 0; i < notes.size() && i < 8; ++i) {
            auto *it = new QListWidgetItem(m_list);
            it->setData(KindRole, 0);
            it->setData(IdRole, notes[i]->id);
            it->setData(TitleRole, notes[i]->title.isEmpty() ? T("无标题") : notes[i]->title);
            it->setData(MetaRole, fmt::shortTime(notes[i]->updatedAt, now));
            it->setData(Qt::DecorationRole, QStringLiteral("file"));
        }
    } else {
        for (const SearchHit &h : searchNotes(m_engine->noteList(), raw, now)) {
            auto *it = new QListWidgetItem(m_list);
            it->setData(KindRole, 0);
            it->setData(IdRole, h.note->id);
            it->setData(TitleRole, h.note->title.isEmpty() ? T("无标题") : h.note->title);
            it->setData(SnippetRole, h.snippet);
            QVariantList marks;
            for (const auto &[a, b] : h.marks) marks << QPoint(a, b);
            it->setData(MarksRole, marks);
            it->setData(MetaRole, fmt::shortTime(h.note->updatedAt, now));
            it->setData(Qt::DecorationRole, QStringLiteral("file"));
        }
        if (m_list->count() == 0) {
            auto *it = new QListWidgetItem(m_list);
            it->setFlags(Qt::NoItemFlags);
            it->setData(TitleRole, T("没有找到笔记"));
            it->setData(Qt::DecorationRole, QStringLiteral("search"));
            it->setData(KindRole, -1);
        }
    }
    if (m_list->count()) m_list->setCurrentRow(0);
}

void CommandPalette::activate() {
    QListWidgetItem *it = m_list->currentItem();
    if (!it) return;
    const int kind = it->data(KindRole).toInt();
    if (kind == 0) {
        const QString id = it->data(IdRole).toString();
        close();
        emit openNote(id);
    } else if (kind == 1) {
        const auto cmd = m_commands.value(it->data(IndexRole).toInt());
        close();
        if (cmd.run) cmd.run();
    }
}

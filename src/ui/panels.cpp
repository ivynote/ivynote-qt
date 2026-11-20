#include "ui/panels.h"

#include "core/agenda.h"
#include "core/format.h"
#include "core/i18n.h"
#include "sync/engine.h"
#include "ui/icons.h"
#include "ui/taskpicker.h"
#include "ui/theme.h"
#include "ui/toast.h"
#include "ui/widgets.h"
#include "vault/session.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QStackedLayout>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace {

enum {
    SubRole = Qt::UserRole + 10, // second line
    SubColorRole,                // QColor for it; the muted ink when unset
    SectionRole,                 // bool: a heading, not a row
    IdRole,                      // note or attachment id
    LineRole,                    // task line
    KindRole,                    // attachment kind
    RawLineRole,                 // original markdown line for safe task mutation
    DoneRole,                    // bool: completed task
    OverdueRole,                 // bool: overdue task
    TodayRole,                   // bool: due today
    DueDateRole,                 // QDate
    DueTimeRole,                 // QString "HH:mm" or empty
    RepeatRuleRole,              // QString raw repeat rule
    BranchKeyRole,               // QString branch key on section rows
    BranchOpenRole,              // bool branch expanded state
    BranchKindRole,              // int AgendaBranch::Kind
    BranchCountRole,             // int branch count badge
};

constexpr int kHeader = 40;
constexpr int kCheckHitRight = 38; // x <= pill.left() + kCheckHitRight hits the checkbox

// Rows with a title and a detail line, selected and hovered like the shelf's.
class RowDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    // Media rows lead with a 36 px tile holding a thumbnail or a type icon.
    bool tiles = false;
    bool agenda = false;
    std::function<QImage(const QModelIndex &)> thumbnail;

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const override {
        if (index.data(SectionRole).toBool()) return {0, 32};
        return {0, tiles ? 52 : 50};
    }

    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const override {
        const Tokens &k = Theme::t();
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        if (index.data(SectionRole).toBool()) {
            if (!agenda) {
                p->setPen(k.muted);
                p->setFont(Theme::uiFont(12, QFont::Medium));
                p->drawText(opt.rect.adjusted(14, 6, -8, 0), Qt::AlignVCenter | Qt::AlignLeft, index.data().toString());
                p->restore();
                return;
            }
            const QRect pill = opt.rect.adjusted(6, 2, -6, -1);
            if (opt.state & (QStyle::State_MouseOver | QStyle::State_Selected)) {
                p->setPen(Qt::NoPen);
                p->setBrush(Theme::alpha(k.ink, 0.045));
                p->drawRoundedRect(pill, Theme::radius, Theme::radius);
            }
            const int bkind = index.data(BranchKindRole).toInt();
            const QColor accent = bkind == int(AgendaBranch::Overdue) ? k.danger
                                  : bkind == int(AgendaBranch::Today) ? k.ivy
                                                                      : k.muted;
            const bool open = index.data(BranchOpenRole).toBool();
            const qreal dpr = p->device()->devicePixelRatioF();
            const QPixmap chev = icons::pixmap(open ? QStringLiteral("chevron-down") : QStringLiteral("chevron-right"), accent, 14, dpr);
            p->drawPixmap(pill.left() + 6, pill.center().y() - 7, chev);

            const int count = index.data(BranchCountRole).toInt();
            const QString countText = QString::number(count);
            const QFont countFont = Theme::uiFont(11, QFont::Medium);
            const int countW = QFontMetrics(countFont).horizontalAdvance(countText) + 12;

            p->setFont(Theme::uiFont(12, QFont::DemiBold));
            p->setPen(accent);
            const QRect labelRect(pill.left() + 24, pill.top(), pill.width() - 32 - countW, pill.height());
            p->drawText(labelRect, Qt::AlignVCenter | Qt::AlignLeft,
                        QFontMetrics(p->font()).elidedText(index.data().toString(), Qt::ElideRight, labelRect.width()));

            p->setFont(countFont);
            p->setPen(accent);
            p->drawText(QRect(pill.right() - countW - 6, pill.top(), countW, pill.height()),
                        Qt::AlignVCenter | Qt::AlignRight, countText);
            p->restore();
            return;
        }
        const QRect pill = opt.rect.adjusted(6, 1, -6, -1);
        const bool selected = opt.state & QStyle::State_Selected;
        if (selected || opt.state & QStyle::State_MouseOver) {
            p->setPen(Qt::NoPen);
            p->setBrush(Theme::alpha(k.ink, selected ? 0.085 : 0.045));
            p->drawRoundedRect(pill, Theme::radius, Theme::radius);
        }
        int left = pill.left() + 10;
        if (agenda) {
            const bool done = index.data(DoneRole).toBool();
            const bool overdue = index.data(OverdueRole).toBool();
            const bool today = index.data(TodayRole).toBool();
            const QRectF box(pill.left() + 10.5, pill.center().y() - 7.5, 15.0, 15.0);
            if (done) {
                p->setPen(Qt::NoPen);
                p->setBrush(k.ivy);
                p->drawRoundedRect(box, 3.5, 3.5);
                QPen ck(Qt::white, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
                p->setPen(ck);
                QPainterPath mark;
                mark.moveTo(box.left() + 3.5, box.center().y() + 0.3);
                mark.lineTo(box.left() + 6.4, box.bottom() - 3.6);
                mark.lineTo(box.right() - 3.3, box.top() + 4.0);
                p->drawPath(mark);
            } else {
                const QColor border = overdue ? k.danger : today ? k.ivy : k.muted;
                p->setPen(QPen(border, 1.5));
                p->setBrush(Qt::NoBrush);
                p->drawRoundedRect(box, 3.5, 3.5);
            }
            left = pill.left() + 34;
        } else if (tiles) {
            const QRect tile(left, pill.center().y() - 18, 36, 36);
            p->setPen(Qt::NoPen);
            p->setBrush(k.sunken);
            p->drawRoundedRect(tile, Theme::radius, Theme::radius);
            const qreal dpr = p->device()->devicePixelRatioF();
            const QImage img = thumbnail ? thumbnail(index) : QImage();
            if (!img.isNull()) {
                const QImage cover = img.scaled(QSize(36, 36) * dpr, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
                QPixmap pm = QPixmap::fromImage(cover.copy((cover.width() - 36 * dpr) / 2, (cover.height() - 36 * dpr) / 2, 36 * dpr, 36 * dpr));
                pm.setDevicePixelRatio(dpr);
                QPainterPath clip;
                clip.addRoundedRect(tile, Theme::radius, Theme::radius);
                p->setClipPath(clip);
                p->drawPixmap(tile.topLeft(), pm);
                p->setClipping(false);
            } else {
                const QString kind = index.data(KindRole).toString();
                const QString name = kind == QLatin1String("image") ? QStringLiteral("image")
                                     : kind == QLatin1String("video") ? QStringLiteral("video")
                                     : kind == QLatin1String("audio") ? QStringLiteral("audio")
                                                                      : QStringLiteral("file");
                p->drawPixmap(tile.center() - QPoint(9, 9), icons::pixmap(name, k.muted, 18, dpr));
            }
            left = tile.right() + 10;
        }
        const QRect text(left, pill.top(), pill.right() - left - 8, pill.height());
        QFont titleFont = Theme::uiFont(13);
        const bool done = agenda && index.data(DoneRole).toBool();
        if (done) titleFont.setStrikeOut(true);
        const QFont subFont = Theme::uiFont(11.5);
        const QFontMetrics tm(titleFont), sm(subFont);
        const int gap = 2;
        const int block = tm.height() + gap + sm.height();
        const int top = text.top() + (text.height() - block) / 2;
        p->setFont(titleFont);
        p->setPen(done ? k.muted : k.ink);
        p->drawText(QRect(text.left(), top, text.width(), tm.height()), Qt::AlignVCenter | Qt::AlignLeft,
                    tm.elidedText(index.data().toString(), Qt::ElideRight, text.width()));
        const QVariant color = index.data(SubColorRole);
        p->setFont(subFont);
        p->setPen(color.isValid() ? color.value<QColor>() : k.muted);
        p->drawText(QRect(text.left(), top + tm.height() + gap, text.width(), sm.height()), Qt::AlignVCenter | Qt::AlignLeft,
                    sm.elidedText(index.data(SubRole).toString(), Qt::ElideRight, text.width()));
        p->restore();
    }
};

class AgendaList : public QListWidget {
public:
    using QListWidget::QListWidget;

    std::function<void(QListWidgetItem *)> onCheckClick;
    std::function<void(QListWidgetItem *)> onTaskClick;
    std::function<void(const QString &)> onBranchToggle;
    std::function<void(const QString &, bool)> onBranchSet;

protected:
    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton) {
            if (QListWidgetItem *item = itemAt(e->pos())) {
                setCurrentItem(item);
                if (item->data(SectionRole).toBool()) {
                    if (onBranchToggle) onBranchToggle(item->data(BranchKeyRole).toString());
                    e->accept();
                    return;
                }
                const QRect r = visualItemRect(item);
                if (e->pos().x() <= r.left() + kCheckHitRight) {
                    if (onCheckClick) onCheckClick(item);
                    e->accept();
                    return;
                }
                if (onTaskClick) onTaskClick(item);
                e->accept();
                return;
            }
        }
        QListWidget::mousePressEvent(e);
    }

    void keyPressEvent(QKeyEvent *e) override {
        QListWidgetItem *item = currentItem();
        if (item) {
            const bool section = item->data(SectionRole).toBool();
            if (e->key() == Qt::Key_Space) {
                if (section) {
                    if (onBranchToggle) onBranchToggle(item->data(BranchKeyRole).toString());
                } else if (onCheckClick) {
                    onCheckClick(item);
                }
                e->accept();
                return;
            }
            if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
                if (section) {
                    if (onBranchToggle) onBranchToggle(item->data(BranchKeyRole).toString());
                } else if (onTaskClick) {
                    onTaskClick(item);
                }
                e->accept();
                return;
            }
            if (section && (e->key() == Qt::Key_Left || e->key() == Qt::Key_Right)) {
                const bool open = item->data(BranchOpenRole).toBool();
                const bool wantOpen = (e->key() == Qt::Key_Right);
                if (open != wantOpen && onBranchSet)
                    onBranchSet(item->data(BranchKeyRole).toString(), wantOpen);
                e->accept();
                return;
            }
        }
        QListWidget::keyPressEvent(e);
    }
};

void styleList(QListWidget *list, RowDelegate *delegate) {
    list->setItemDelegate(delegate);
    list->setFrameShape(QFrame::NoFrame);
    list->setMouseTracking(true);
    list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setAttribute(Qt::WA_MacShowFocusRect, false);
    list->setContextMenuPolicy(Qt::CustomContextMenu);
    list->viewport()->setAutoFillBackground(false);
}

QListWidget *makeList(QWidget *parent, RowDelegate *delegate) {
    auto *list = new QListWidget(parent);
    styleList(list, delegate);
    return list;
}

// A page for "nothing here": a muted line in the middle.
QLabel *makeEmpty(const QString &text, QWidget *parent) {
    auto *l = mutedLabel(text, parent, 13);
    l->setAlignment(Qt::AlignCenter);
    l->setWordWrap(true);
    l->setMargin(24);
    return l;
}

QTimer *debounce(QTimer *t, QObject *owner, std::function<void()> fn) {
    t->setSingleShot(true);
    t->setInterval(120);
    QObject::connect(t, &QTimer::timeout, owner, std::move(fn));
    return t;
}

QDate nextMonday(const QDate &from) {
    const int dow = from.dayOfWeek(); // 1=Mon .. 7=Sun
    const int delta = (8 - dow) % 7;
    return from.addDays(delta == 0 ? 7 : delta);
}

} // namespace

// --- header ---

PanelHeader::PanelHeader(const QString &title, QWidget *parent) : QWidget(parent) {
    setFixedHeight(kHeader);
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(14, 0, 8, 0);
    row->setSpacing(6);
    auto *label = new QLabel(title, this);
    label->setFont(Theme::uiFont(14.5, QFont::DemiBold));
    row->addWidget(label);
    m_count = mutedLabel(QString(), this, 12);
    row->addWidget(m_count);
    row->addStretch(1);
}

void PanelHeader::setCount(int n) {
    m_count->setText(n >= 0 ? QString::number(n) : QString());
    m_count->setVisible(n >= 0);
}

QLayout *PanelHeader::actions() const { return layout(); }

// --- agenda ---

AgendaPanel::AgendaPanel(QWidget *parent) : QWidget(parent) {
    loadState();

    auto *col = new QVBoxLayout(this);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);
    m_header = new PanelHeader(T("日程"), this);
    m_doneToggle = new IconButton(QStringLiteral("todo"), T("显示今天已完成"), m_header);
    m_doneToggle->setCheckable(true);
    m_doneToggle->setChecked(m_showDone);
    connect(m_doneToggle, &QToolButton::toggled, this, &AgendaPanel::setShowDone);
    m_header->actions()->addWidget(m_doneToggle);
    col->addWidget(m_header);

    m_summary = mutedLabel(QString(), this, 11.5);
    m_summary->setContentsMargins(14, 0, 12, 6);
    m_summary->hide();
    col->addWidget(m_summary);

    auto *delegate = new RowDelegate(this);
    delegate->agenda = true;
    auto *alist = new AgendaList(this);
    styleList(alist, delegate);
    alist->onCheckClick = [this](QListWidgetItem *item) {
        if (!item || item->data(SectionRole).toBool()) return;
        emit taskToggled(item->data(IdRole).toString(), item->data(LineRole).toInt(), item->data(RawLineRole).toString());
    };
    alist->onTaskClick = [this](QListWidgetItem *item) {
        if (!item || item->data(SectionRole).toBool()) return;
        emit taskActivated(item->data(IdRole).toString(), item->data(LineRole).toInt());
    };
    alist->onBranchToggle = [this](const QString &key) { toggleBranch(key); };
    alist->onBranchSet = [this](const QString &key, bool) { toggleBranch(key); };
    m_list = alist;
    connect(m_list, &QListWidget::customContextMenuRequested, this, &AgendaPanel::showTaskMenu);

    m_empty = makeEmpty(T("没有待办日程") + QStringLiteral("\n") + T("在笔记里写下“- [ ] 任务 📅 2026-01-31 09:00”就会出现在这里。"), this);
    auto *host = new QWidget(this);
    m_pages = new QStackedLayout(host);
    m_pages->addWidget(m_list);
    m_pages->addWidget(m_empty);
    col->addWidget(host, 1);

    m_encryptedHint = mutedLabel(QString(), this, 11.5);
    m_encryptedHint->setContentsMargins(14, 6, 12, 8);
    m_encryptedHint->setWordWrap(true);
    m_encryptedHint->hide();
    col->addWidget(m_encryptedHint);

    debounce(&m_refresh, this, [this] { refresh(); });
    auto *minute = new QTimer(this);
    connect(minute, &QTimer::timeout, this, &AgendaPanel::refresh);
    minute->start(60000);
    connect(Theme::instance(), &Theme::changed, this, &AgendaPanel::refresh);
}

void AgendaPanel::loadState() {
    QSettings s;
    m_showDone = s.value(QStringLiteral("agenda/showDone"), false).toBool();
    const QStringList list = s.value(QStringLiteral("agenda/toggled")).toStringList();
    m_toggledBranches = QSet<QString>(list.begin(), list.end());
}

void AgendaPanel::saveState() const {
    QSettings s;
    s.setValue(QStringLiteral("agenda/showDone"), m_showDone);
    s.setValue(QStringLiteral("agenda/toggled"), QStringList(m_toggledBranches.begin(), m_toggledBranches.end()));
}

void AgendaPanel::setShowDone(bool show) {
    if (m_showDone == show) return;
    m_showDone = show;
    if (m_doneToggle->isChecked() != show) m_doneToggle->setChecked(show);
    saveState();
    refresh();
}

void AgendaPanel::toggleBranch(const QString &key) {
    if (key.isEmpty()) return;
    if (m_toggledBranches.contains(key)) m_toggledBranches.remove(key);
    else m_toggledBranches.insert(key);
    saveState();
    refresh();
}

void AgendaPanel::setSession(Session *session) {
    if (m_session) m_session->engine()->disconnect(this);
    m_session = session;
    if (session) connect(session->engine(), &SyncEngine::dataChanged, this, [this] { m_refresh.start(); });
    refresh();
}

void AgendaPanel::refresh() {
    QString selectedKey;
    if (QListWidgetItem *cur = m_list->currentItem()) {
        if (cur->data(SectionRole).toBool())
            selectedKey = QStringLiteral("b:") + cur->data(BranchKeyRole).toString();
        else
            selectedKey = QStringLiteral("t:%1:%2").arg(cur->data(IdRole).toString()).arg(cur->data(LineRole).toInt());
    }

    m_list->clear();
    if (!m_session) {
        m_header->setCount(-1);
        m_summary->hide();
        m_encryptedHint->hide();
        m_pages->setCurrentWidget(m_empty);
        emit badgeChanged(0, false);
        return;
    }

    const Tokens &k = Theme::t();
    const QDateTime now = QDateTime::currentDateTime();
    const bool zh = i18n::isZh();
    const AgendaTree tree = buildAgendaTree(m_session->engine()->notes(), m_showDone, now, zh);

    // Summary line
    QStringList stats;
    if (tree.overdueCount > 0)
        stats << (zh ? QStringLiteral("已过期 %1").arg(tree.overdueCount) : QStringLiteral("%1 overdue").arg(tree.overdueCount));
    if (tree.todayCount > 0)
        stats << (zh ? QStringLiteral("今天 %1").arg(tree.todayCount) : QStringLiteral("%1 today").arg(tree.todayCount));
    if (!stats.isEmpty()) {
        m_summary->setText(stats.join(QStringLiteral(" · ")));
        QPalette pal = m_summary->palette();
        pal.setColor(QPalette::WindowText, tree.overdueCount > 0 ? k.danger : k.ivy);
        m_summary->setPalette(pal);
        m_summary->show();
    } else {
        m_summary->hide();
    }

    const QString dateFormat = zh ? QStringLiteral("M月d日") : QStringLiteral("MMM d");
    int shownRows = 0;
    for (const AgendaBranch &b : tree.branches) {
        const bool isOpen = b.openByDefault() != m_toggledBranches.contains(b.key);
        auto *section = new QListWidgetItem(b.label, m_list);
        section->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        section->setData(SectionRole, true);
        section->setData(BranchKeyRole, b.key);
        section->setData(BranchOpenRole, isOpen);
        section->setData(BranchKindRole, int(b.kind));
        section->setData(BranchCountRole, int(b.items.size()));
        if (selectedKey == QStringLiteral("b:") + b.key) m_list->setCurrentItem(section);
        ++shownRows;
        if (!isOpen) continue;

        const bool isOverdue = (b.kind == AgendaBranch::Overdue);
        const bool isToday = (b.kind == AgendaBranch::Today);
        const bool showDate = (b.kind == AgendaBranch::Overdue || b.kind == AgendaBranch::Month);

        for (const AgendaTask &t : b.items) {
            auto *item = new QListWidgetItem(t.text.isEmpty() ? T("无标题") : t.text, m_list);
            const QString when = t.allDay ? T("全天") : t.due.toString(QStringLiteral("HH:mm"));
            QString sub = showDate ? (t.due.date().toString(dateFormat) + (t.allDay ? QString() : QStringLiteral(" ") + when)) : when;
            if (t.repeat) sub += QStringLiteral(" · 🔁 ") + tasks::describeRepeat(*t.repeat, zh);
            const QString note = t.title.trimmed().isEmpty() ? T("无标题") : t.title;
            sub += QStringLiteral(" · ") + note;

            item->setData(SubRole, sub);
            if (!t.done && isOverdue) item->setData(SubColorRole, k.danger);
            else if (!t.done && isToday) item->setData(SubColorRole, k.ivy);
            item->setData(IdRole, t.noteId);
            item->setData(LineRole, t.line);
            item->setData(RawLineRole, t.rawLine);
            item->setData(DoneRole, t.done);
            item->setData(OverdueRole, isOverdue);
            item->setData(TodayRole, isToday);
            item->setData(DueDateRole, t.due.date());
            item->setData(DueTimeRole, t.allDay ? QTime() : t.due.time());
            item->setData(RepeatRuleRole, t.repeat ? t.repeat->raw : QString());
            item->setToolTip(note + u'\n' + t.rawLine.trimmed());
            if (selectedKey == QStringLiteral("t:%1:%2").arg(t.noteId).arg(t.line)) m_list->setCurrentItem(item);
            ++shownRows;
        }
    }

    if (tree.encryptedCount > 0) {
        m_encryptedHint->setText(
            (zh ? QStringLiteral("已跳过 %1 篇加密笔记中的任务") : QStringLiteral("%1 encrypted notes skipped")).arg(tree.encryptedCount));
        m_encryptedHint->show();
    } else {
        m_encryptedHint->hide();
    }

    m_header->setCount(tree.totalOpen);
    m_pages->setCurrentWidget(shownRows == 0 ? static_cast<QWidget *>(m_empty) : static_cast<QWidget *>(m_list));
    emit badgeChanged(tree.overdueCount + tree.todayCount, tree.overdueCount > 0);
}

void AgendaPanel::showTaskMenu(const QPoint &pos) {
    QListWidgetItem *item = m_list->itemAt(pos);
    if (!item || item->data(SectionRole).toBool() || !m_session) return;
    m_list->setCurrentItem(item);

    const QString noteId = item->data(IdRole).toString();
    const int line = item->data(LineRole).toInt();
    const QString rawLine = item->data(RawLineRole).toString();
    const bool done = item->data(DoneRole).toBool();
    const QDate dueDate = item->data(DueDateRole).toDate();
    const QTime dueTime = item->data(DueTimeRole).toTime();
    const QString repeatRaw = item->data(RepeatRuleRole).toString();

    QMenu menu(this);
    menu.addAction(icons::themed("check"), done ? T("取消完成") : T("标记完成"), this, [this, noteId, line, rawLine] {
        emit taskToggled(noteId, line, rawLine);
    });
    menu.addSeparator();
    menu.addAction(icons::themed("calendar"), T("改期与重复…"), this, [this, noteId, line, rawLine, dueDate, dueTime, repeatRaw] {
        std::optional<tasks::Due> initDue;
        if (dueDate.isValid()) initDue = tasks::Due{dueDate, dueTime};
        std::optional<tasks::RepeatRule> initRep;
        if (!repeatRaw.isEmpty()) initRep = tasks::parseRepeat(repeatRaw);
        TaskDueDialog dlg(initDue, initRep, this);
        dlg.exec();
        if (dlg.action() == TaskDueDialog::Applied)
            emit taskDueChanged(noteId, line, rawLine, dlg.selectedDue(), dlg.selectedRepeat());
        else if (dlg.action() == TaskDueDialog::Removed)
            emit taskDueChanged(noteId, line, rawLine, std::nullopt, QString());
    });

    const QDate today = QDate::currentDate();
    const auto reschedule = [this, noteId, line, rawLine, dueTime, repeatRaw](const QDate &d) {
        emit taskDueChanged(noteId, line, rawLine, tasks::Due{d, dueTime}, repeatRaw);
    };
    menu.addAction(T("改到今天"), this, [reschedule, today] { reschedule(today); });
    menu.addAction(T("改到明天"), this, [reschedule, today] { reschedule(today.addDays(1)); });
    menu.addAction(T("改到下周一"), this, [reschedule, today] { reschedule(nextMonday(today)); });

    menu.addSeparator();
    menu.addAction(icons::themed("note"), T("打开笔记"), this, [this, noteId, line] {
        emit taskActivated(noteId, line);
    });
    menu.exec(m_list->viewport()->mapToGlobal(pos));
}

// --- media library ---

MediaPanel::MediaPanel(QWidget *parent) : QWidget(parent) {
    auto *col = new QVBoxLayout(this);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);
    m_header = new PanelHeader(T("媒体库"), this);
    col->addWidget(m_header);

    auto *filters = new QHBoxLayout;
    filters->setContentsMargins(10, 0, 10, 8);
    filters->setSpacing(6);
    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(T("筛选附件"));
    m_filter->setClearButtonEnabled(true);
    filters->addWidget(m_filter, 1);
    m_kind = new QComboBox(this);
    m_kind->addItem(T("全部"), QString());
    m_kind->addItem(T("图片"), QStringLiteral("image"));
    m_kind->addItem(T("视频"), QStringLiteral("video"));
    m_kind->addItem(T("音频"), QStringLiteral("audio"));
    m_kind->addItem(T("文件"), QStringLiteral("file"));
    filters->addWidget(m_kind);
    col->addLayout(filters);

    auto *delegate = new RowDelegate(this);
    delegate->tiles = true;
    delegate->thumbnail = [this](const QModelIndex &index) -> QImage {
        if (!m_session || index.data(KindRole).toString() != QLatin1String("image")) return {};
        return m_session->image(index.data(IdRole).toString(), true);
    };
    m_list = makeList(this, delegate);
    m_empty = makeEmpty(T("还没有附件"), this);
    auto *host = new QWidget(this);
    m_pages = new QStackedLayout(host);
    m_pages->addWidget(m_list);
    m_pages->addWidget(m_empty);
    col->addWidget(host, 1);

    debounce(&m_refresh, this, [this] { refresh(); });
    connect(m_filter, &QLineEdit::textChanged, this, &MediaPanel::refresh);
    connect(m_kind, &QComboBox::currentIndexChanged, this, &MediaPanel::refresh);
    connect(m_list, &QListWidget::itemActivated, this, &MediaPanel::open);
    connect(m_list, &QListWidget::itemDoubleClicked, this, &MediaPanel::open);
    connect(m_list, &QListWidget::customContextMenuRequested, this, &MediaPanel::menu);
    connect(Theme::instance(), &Theme::changed, this, [this] { m_list->viewport()->update(); });
}

void MediaPanel::setSession(Session *session) {
    if (m_session) {
        m_session->engine()->disconnect(this);
        m_session->disconnect(this);
    }
    m_session = session;
    if (session) {
        connect(session->engine(), &SyncEngine::dataChanged, this, [this] { m_refresh.start(); });
        connect(session, &Session::attachmentReady, this, [this] { m_list->viewport()->update(); });
    }
    refresh();
}

void MediaPanel::refresh() {
    const QString selected = m_list->currentItem() ? m_list->currentItem()->data(IdRole).toString() : QString();
    m_list->clear();
    if (!m_session) {
        m_header->setCount(-1);
        m_pages->setCurrentWidget(m_empty);
        return;
    }
    QList<const Attachment *> items;
    for (const Attachment &a : m_session->engine()->attachments())
        if (!a.deleted && a.trashedAt <= 0) items.append(&a);
    std::sort(items.begin(), items.end(), [](const Attachment *a, const Attachment *b) {
        return std::tie(b->createdAt, a->name, a->id) < std::tie(a->createdAt, b->name, b->id);
    });
    const QString needle = m_filter->text().trimmed();
    const QString kind = m_kind->currentData().toString();
    int shown = 0;
    for (const Attachment *a : items) {
        const QString k = a->kind == QLatin1String("image") || a->kind == QLatin1String("video") || a->kind == QLatin1String("audio")
                              ? a->kind
                              : QStringLiteral("file");
        if (!kind.isEmpty() && k != kind) continue;
        if (!needle.isEmpty() && !a->name.contains(needle, Qt::CaseInsensitive)) continue;
        auto *row = new QListWidgetItem(a->name, m_list);
        const QString label = k == QLatin1String("image") ? T("图片") : k == QLatin1String("video") ? T("视频") : k == QLatin1String("audio") ? T("音频") : T("文件");
        row->setData(SubRole, label + QStringLiteral(" · ") + fmt::bytes(a->size));
        row->setData(IdRole, a->id);
        row->setData(KindRole, k);
        row->setToolTip(a->name + QStringLiteral("\n") + a->mime);
        if (a->id == selected) m_list->setCurrentItem(row);
        ++shown;
    }
    m_header->setCount(int(items.size()));
    m_empty->setText(items.isEmpty() ? T("还没有附件") : T("没有符合条件的附件"));
    m_pages->setCurrentWidget(shown ? static_cast<QWidget *>(m_list) : m_empty);
}

void MediaPanel::open() {
    if (m_session && m_list->currentItem()) m_session->openAttachment(m_list->currentItem()->data(IdRole).toString());
}

void MediaPanel::menu(const QPoint &pos) {
    QListWidgetItem *item = m_list->itemAt(pos);
    if (!item || !m_session) return;
    m_list->setCurrentItem(item);
    const QString id = item->data(IdRole).toString();
    QMenu menu(this);
    menu.addAction(icons::themed("eye"), T("打开"), this, &MediaPanel::open);
    menu.addAction(icons::themed("download"), T("另存为…"), this, [this, id] {
        if (m_session) m_session->saveAttachmentAs(id, this);
    });
    menu.addAction(icons::themed("copy"), T("复制文件名"), this, [name = item->text()] {
        QApplication::clipboard()->setText(name);
        Toaster::show(T("已复制"));
    });
    menu.exec(m_list->viewport()->mapToGlobal(pos));
}

// --- sync indicator ---

SyncIndicator::SyncIndicator(QWidget *parent) : QToolButton(parent) {
    setAutoRaise(true);
    setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    setFont(Theme::uiFont(12));
    setCursor(Qt::PointingHandCursor);
    setIconSize(QSize(12, 12));
    m_clock.setInterval(30000);
    connect(&m_clock, &QTimer::timeout, this, &SyncIndicator::refreshState);
    m_clock.start();
    connect(Theme::instance(), &Theme::changed, this, &SyncIndicator::refreshState);
    hide();
}

void SyncIndicator::setSession(Session *session) {
    if (m_session) {
        if (m_engine) m_engine->disconnect(this);
        m_session->disconnect(this);
    }
    m_session = session;
    m_engine = session ? session->engine() : nullptr;
    if (session) {
        connect(m_engine, &SyncEngine::stateChanged, this, &SyncIndicator::refreshState);
        connect(session, &Session::userChanged, this, &SyncIndicator::refreshState);
        connect(session, &Session::needsSignIn, this, &SyncIndicator::refreshState);
    }
    refreshState();
}

void SyncIndicator::refreshState() {
    const Tokens &k = Theme::t();
    QColor dot;
    QString text;
    QString tip;
    if (!m_session || !m_engine) {
        setVisible(false);
        return;
    }
    if (!m_session->isRemote()) {
        text = T("本地笔记库 · 仅保存在本机");
    } else {
        const SyncState &s = m_engine->state();
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const int waiting = s.pending + s.uploads;
        if (!m_session->signedIn() || m_engine->paused()) {
            text = T("需要重新登录 · 更改保存在本机");
            dot = k.danger;
        } else if (s.blocked > 0) {
            text = (i18n::isZh() ? QStringLiteral("%1 条更改被服务器拒绝") : QStringLiteral("%1 changes refused by the server")).arg(s.blocked);
            dot = k.danger;
        } else if (s.status == SyncState::Syncing) {
            text = T("同步中…");
            dot = k.ivy;
        } else if (s.status == SyncState::Offline) {
            text = waiting > 0 ? (i18n::isZh() ? QStringLiteral("离线中 · %1 条更改将在联网后自动同步") : QStringLiteral("Offline · %1 changes will sync when online")).arg(waiting)
                               : T("离线中");
            dot = k.warn;
        } else if (s.status == SyncState::Error) {
            text = T("同步出错");
            dot = k.danger;
            if (!s.error.isEmpty() && s.error != QLatin1String("unauthorized")) tip = s.error;
        } else if (waiting > 0) {
            text = (i18n::isZh() ? QStringLiteral("%1 条更改待同步") : QStringLiteral("%1 changes waiting")).arg(waiting);
            dot = k.warn;
        } else {
            text = T("已同步") + QStringLiteral(" · ") + fmt::ago(s.lastSyncAt, now);
            dot = k.ok;
        }
        if (tip.isEmpty()) tip = T("点击立即同步");
    }
    const qreal dpr = devicePixelRatioF();
    if (dot.isValid()) {
        QPixmap pm(QSize(12, 12) * dpr);
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(dot);
        p.drawEllipse(QRectF(3, 3, 6, 6));
        p.end();
        setIcon(QIcon(pm));
    } else {
        setIcon(icons::themed("database"));
    }
    QPalette pal = palette();
    pal.setColor(QPalette::ButtonText, k.muted);
    setPalette(pal);
    setText(text);
    setToolTip(tip.isEmpty() ? text : tip);
    // A local vault has nothing to sync: the line only informs, so it neither
    // greys out nor takes clicks.
    setAttribute(Qt::WA_TransparentForMouseEvents, !m_session->isRemote());
    setFocusPolicy(m_session->isRemote() ? Qt::TabFocus : Qt::NoFocus);
    setCursor(m_session->isRemote() ? Qt::PointingHandCursor : Qt::ArrowCursor);
    setVisible(true);
}

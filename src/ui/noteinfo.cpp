#include "ui/noteinfo.h"

#include "core/format.h"
#include "core/i18n.h"
#include "core/markdown.h"
#include "core/models.h"
#include "core/notelock.h"
#include "sync/api.h"
#include "sync/engine.h"
#include "ui/icons.h"
#include "ui/theme.h"
#include "ui/toast.h"
#include "ui/widgets.h"
#include "vault/session.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLocale>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace {

QString when(qint64 ms) {
    if (!ms) return QStringLiteral("—");
    const QDateTime t = QDateTime::fromMSecsSinceEpoch(ms);
    return i18n::isZh() ? t.toString(QStringLiteral("yyyy年M月d日 HH:mm")) : QLocale(QLocale::English).toString(t, QStringLiteral("MMM d, yyyy HH:mm"));
}

QString deviceLabel(const QString &d) {
    const QString kind = d.startsWith(QLatin1String("web")) ? QStringLiteral("Web")
                         : d.startsWith(QLatin1String("desktop")) ? QStringLiteral("Desktop")
                         : d.startsWith(QLatin1String("mcp")) || d.startsWith(QLatin1String("api")) ? QStringLiteral("API")
                                                                                                    : QString();
    if (kind.isEmpty() || kind.toLower() == d) return d;
    QString rest = d;
    rest.remove(QRegularExpression(QStringLiteral("^(web|desktop)-?")));
    return kind + QStringLiteral(" · ") + (rest.isEmpty() ? d : rest);
}

QLabel *text(const QString &s, QWidget *parent, qreal px = 13.5, bool muted = false) {
    if (muted) return mutedLabel(s, parent, px);
    auto *l = new QLabel(s, parent);
    l->setFont(Theme::uiFont(px));
    return l;
}

// One-pixel rule between rows.
class Rule : public QWidget {
public:
    explicit Rule(QWidget *parent) : QWidget(parent) {
        setFixedHeight(1);
        connect(Theme::instance(), &Theme::changed, this, qOverload<>(&QWidget::update));
    }

protected:
    void paintEvent(QPaintEvent *) override { QPainter(this).fillRect(rect(), Theme::t().line); }
};

// A figure over its caption, on a sunken tile.
class Tile : public QFrame {
public:
    Tile(const QString &value, const QString &caption, QWidget *parent) : QFrame(parent) {
        auto *col = new QVBoxLayout(this);
        col->setContentsMargins(12, 9, 12, 9);
        col->setSpacing(1);
        auto *v = text(value, this, 16);
        QFont f = v->font();
        f.setWeight(QFont::Medium);
        v->setFont(f);
        col->addWidget(v);
        col->addWidget(text(caption, this, 12, true));
        connect(Theme::instance(), &Theme::changed, this, qOverload<>(&QWidget::update));
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::t().sunken);
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), Theme::radius + 2, Theme::radius + 2);
    }
};

class Bar : public QWidget {
public:
    Bar(qreal fraction, QWidget *parent) : QWidget(parent), m_fraction(fraction) {
        setFixedHeight(6);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        connect(Theme::instance(), &Theme::changed, this, qOverload<>(&QWidget::update));
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::t().sunken);
        p.drawRoundedRect(rect(), 3, 3);
        p.setBrush(Theme::t().ivy);
        p.drawRoundedRect(QRectF(0, 0, std::max<qreal>(6, width() * m_fraction), height()), 3, 3);
    }

private:
    qreal m_fraction;
};

class Avatar : public QWidget {
public:
    Avatar(const QString &name, QWidget *parent) : QWidget(parent), m_initial(name.trimmed().left(1).toUpper()) {
        setFixedSize(22, 22);
        connect(Theme::instance(), &Theme::changed, this, qOverload<>(&QWidget::update));
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::t().ivySoft);
        p.drawEllipse(rect());
        p.setPen(Theme::t().ivyInk);
        p.setFont(Theme::uiFont(11, QFont::DemiBold));
        p.drawText(rect(), Qt::AlignCenter, m_initial);
    }

private:
    QString m_initial;
};

QPushButton *linkButton(const QString &label, QWidget *parent) {
    auto *b = new QPushButton(label, parent);
    b->setFlat(true);
    b->setCursor(Qt::PointingHandCursor);
    b->setFont(Theme::uiFont(12.5));
    QPalette pal = b->palette();
    pal.setColor(QPalette::ButtonText, Theme::t().ivy);
    b->setPalette(pal);
    b->setStyleSheet(QStringLiteral("QPushButton{border:none;padding:2px 0;background:transparent;}QPushButton:hover{text-decoration:underline;}"));
    return b;
}

QWidget *row(const QString &label, QWidget *value, QWidget *parent) {
    auto *w = new QWidget(parent);
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(0, 8, 0, 8);
    h->setSpacing(14);
    auto *l = text(label, w, 13, true);
    l->setFixedWidth(92);
    l->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    h->addWidget(l);
    h->addWidget(value, 1);
    return w;
}

QLabel *value(const QString &s, QWidget *parent) {
    auto *l = text(s, parent);
    l->setWordWrap(true);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return l;
}

QWidget *section(const QString &title, const QList<QWidget *> &rows, QWidget *parent) {
    auto *w = new QWidget(parent);
    auto *col = new QVBoxLayout(w);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);
    auto *head = text(title, w, 12, true);
    QFont f = head->font();
    f.setWeight(QFont::DemiBold);
    head->setFont(f);
    head->setContentsMargins(0, 0, 0, 2);
    col->addWidget(head);
    for (int i = 0; i < rows.size(); ++i) {
        if (i) col->addWidget(new Rule(w));
        col->addWidget(rows[i]);
    }
    return w;
}

} // namespace

NoteInfoDialog::NoteInfoDialog(Session *session, const QString &noteId, QWidget *parent)
    : QDialog(parent), m_session(session), m_noteId(noteId) {
    setWindowTitle(T("笔记信息"));
    resize(480, 640);
    SyncEngine *engine = session->engine();
    const Note *n = engine->note(noteId);
    if (!n) return;
    const Note note = *n;
    const bool remote = session->isRemote();
    const bool sealed = notelock::isEncrypted(note.body);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto *scroll = new QScrollArea(this);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *page = new QWidget;
    scroll->setWidget(page);
    scroll->viewport()->setAutoFillBackground(false);
    page->setAutoFillBackground(false);
    outer->addWidget(scroll, 1);
    auto *col = new QVBoxLayout(page);
    col->setContentsMargins(24, 22, 24, 18);
    col->setSpacing(20);

    // Title.
    auto *title = new QLabel(page);
    title->setFont(Theme::serif(20));
    title->setText(title->fontMetrics().elidedText(note.title.trimmed().isEmpty() ? T("无标题") : note.title, Qt::ElideRight, 420));
    col->addWidget(title);

    // General.
    QStringList path;
    for (const Folder *f : engine->folderPath(note.folderId)) path << f->name;
    QStringList tags;
    for (const QString &id : note.tags)
        if (auto it = engine->tags().constFind(id); it != engine->tags().constEnd()) tags << QStringLiteral("#") + it->name;
    QString owner = remote ? (!session->user().displayName.isEmpty() ? session->user().displayName : session->user().username) : session->info().name;

    QList<QWidget *> general;
    general << row(T("创建时间"), value(when(note.createdAt), page), page);
    general << row(T("更新时间"), value(when(note.updatedAt), page), page);
    auto *who = new QWidget(page);
    auto *whoRow = new QHBoxLayout(who);
    whoRow->setContentsMargins(0, 0, 0, 0);
    whoRow->setSpacing(8);
    whoRow->addWidget(new Avatar(owner, who));
    whoRow->addWidget(text(owner.isEmpty() ? QStringLiteral("—") : owner, who));
    if (remote && !session->user().username.isEmpty()) whoRow->addWidget(text(QStringLiteral("@") + session->user().username, who, 12, true));
    whoRow->addStretch(1);
    general << row(T("所有者"), who, page);
    general << row(T("位置"), value(path.isEmpty() ? T("不放入笔记本") : path.join(QStringLiteral(" / ")), page), page);
    general << row(T("标签"), value(tags.isEmpty() ? T("无") : tags.join(QStringLiteral("  ")), page), page);
    general << row(T("状态"), value(note.trashed() ? T("在回收站") : note.pinned ? T("已置顶") : T("正常"), page), page);
    QString sync = !remote ? T("仅保存在本机笔记库") : engine->hasPending(EntityType::Note, noteId) ? T("待同步") : T("已同步");
    if (remote && note.rev) sync += QStringLiteral("  ") + T("第 %1 版").arg(note.rev);
    general << row(T("同步"), value(sync, page), page);
    if (remote) {
        auto *shareHost = new QWidget(page);
        auto *shareRow = new QHBoxLayout(shareHost);
        shareRow->setContentsMargins(0, 0, 0, 0);
        shareRow->setSpacing(8);
        m_share = text(QStringLiteral("…"), shareHost);
        m_share->setWordWrap(true);
        shareRow->addWidget(m_share, 1);
        if (!note.trashed() && !sealed) {
            auto *go = linkButton(T("分享"), shareHost);
            connect(go, &QPushButton::clicked, this, [this] {
                accept();
                emit shareRequested();
            });
            shareRow->addWidget(go, 0, Qt::AlignTop);
        }
        general << row(T("分享"), shareHost, page);
    }
    col->addWidget(section(T("基本"), general, page));

    // Statistics.
    auto *stats = new QWidget(page);
    auto *statsCol = new QVBoxLayout(stats);
    statsCol->setContentsMargins(0, 0, 0, 0);
    statsCol->setSpacing(10);
    auto *statsHead = text(T("统计"), stats, 12, true);
    QFont hf = statsHead->font();
    hf.setWeight(QFont::DemiBold);
    statsHead->setFont(hf);
    statsCol->addWidget(statsHead);
    if (sealed) {
        auto *hint = text(T("加密笔记的正文只在你的设备上解密，这里不统计内容。"), stats, 13, true);
        hint->setWordWrap(true);
        statsCol->addWidget(hint);
    } else {
        const md::NoteStats s = md::noteStats(note.body);
        const int backlinks = md::buildLinkIndex(engine->noteList()).backlinks.value(noteId).size();
        auto *grid = new QGridLayout;
        grid->setSpacing(8);
        const QPair<QString, QString> cells[] = {
            {fmt::number(s.words), T("字数")},
            {fmt::number(s.chars), T("字符数")},
            {s.readMinutes ? T("约 %1 分钟").arg(s.readMinutes) : QStringLiteral("—"), T("阅读时长")},
            {fmt::number(s.paragraphs), T("段落")},
            {fmt::number(s.media), T("图片与附件")},
            {QStringLiteral("%1 / %2").arg(fmt::number(s.links), fmt::number(backlinks)), T("链接 / 反链")},
        };
        for (int i = 0; i < 6; ++i) grid->addWidget(new Tile(cells[i].first, cells[i].second, stats), i / 3, i % 3);
        for (int c = 0; c < 3; ++c) grid->setColumnStretch(c, 1);
        statsCol->addLayout(grid);
        if (s.todos) {
            auto *todo = new QHBoxLayout;
            todo->setSpacing(10);
            todo->addWidget(text(T("待办"), stats, 12.5, true));
            todo->addWidget(new Bar(qreal(s.todosDone) / s.todos, stats), 1);
            todo->addWidget(text(T("%1/%2 已完成").arg(s.todosDone).arg(s.todos), stats, 12.5, true));
            statsCol->addLayout(todo);
        }
    }
    col->addWidget(stats);

    // History and devices.
    if (remote) {
        auto *historyHost = new QWidget(page);
        auto *historyRow = new QHBoxLayout(historyHost);
        historyRow->setContentsMargins(0, 0, 0, 0);
        m_revisions = text(QStringLiteral("…"), historyHost);
        historyRow->addWidget(m_revisions, 1);
        auto *open = linkButton(T("查看历史版本"), historyHost);
        connect(open, &QPushButton::clicked, this, [this] {
            accept();
            emit historyRequested();
        });
        historyRow->addWidget(open, 0, Qt::AlignTop);
        auto *devicesHost = new QWidget(page);
        m_devices = new QVBoxLayout(devicesHost);
        m_devices->setContentsMargins(0, 0, 0, 0);
        m_devices->setSpacing(4);
        m_devices->addWidget(text(QStringLiteral("…"), devicesHost, 13.5, true));
        col->addWidget(section(T("历史与设备"), {row(T("历史版本"), historyHost, page), row(T("编辑过的设备"), devicesHost, page)}, page));
    }
    col->addStretch(1);

    // Footer: the id, and close.
    auto *footer = new QWidget(this);
    auto *foot = new QHBoxLayout(footer);
    foot->setContentsMargins(24, 10, 24, 14);
    foot->setSpacing(8);
    foot->addWidget(text(T("笔记 ID"), footer, 12, true));
    auto *id = new QLabel(note.id, footer);
    id->setFont(Theme::mono(11.5));
    id->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QPalette ip = id->palette();
    ip.setColor(QPalette::WindowText, Theme::t().muted);
    id->setPalette(ip);
    foot->addWidget(id, 1);
    auto *copy = new IconButton(QStringLiteral("copy"), T("复制 ID"), footer);
    connect(copy, &QToolButton::clicked, this, [noteId = note.id] {
        QApplication::clipboard()->setText(noteId);
        Toaster::show(T("已复制"));
    });
    foot->addWidget(copy);
    auto *close = new QPushButton(T("关闭"), footer);
    close->setDefault(true);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    foot->addWidget(close);
    outer->addWidget(new Rule(this));
    outer->addWidget(footer);

    if (remote) loadRemote();
}

void NoteInfoDialog::loadRemote() {
    QPointer<NoteInfoDialog> self(this);
    const QString offline = T("联网后可查看分享与历史信息");
    m_session->api()->get(QStringLiteral("/notes/%1/share").arg(m_noteId), this, [self, offline](const ApiReply &r) {
        if (!self || !self->m_share) return;
        if (r.transient()) {
            self->m_share->setText(offline);
            return;
        }
        const QJsonObject s = r.ok ? r.json.toObject() : QJsonObject();
        QString line;
        if (s.isEmpty() || !s.value("enabled").toBool()) line = T("未分享");
        else if (!s.value("hasPassword").toBool()) line = T("链接未设置密码，已暂停访问");
        else if (s.value("expired").toBool()) line = T("链接已过期");
        else line = T("公开链接（有密码）· %1 次访问").arg(s.value("views").toInt());
        self->m_share->setText(line);
    });
    m_session->api()->get(QStringLiteral("/notes/%1/history").arg(m_noteId), this, [self, offline](const ApiReply &r) {
        if (!self || !self->m_revisions) return;
        if (!r.ok) {
            self->m_revisions->setText(r.transient() ? offline : T("无"));
            while (QLayoutItem *it = self->m_devices->takeAt(0)) {
                delete it->widget();
                delete it;
            }
            self->m_devices->addWidget(text(QStringLiteral("—"), self, 13.5, true));
            return;
        }
        const QJsonArray revs = r.json.toArray();
        self->m_revisions->setText(T("%1 个版本").arg(revs.size()));
        // Most recent first, each device once.
        QList<QPair<QString, qint64>> devices;
        QSet<QString> seen;
        for (const QJsonValue &v : revs) {
            const QJsonObject o = v.toObject();
            const QString d = o.value("device").toString();
            if (d.isEmpty() || seen.contains(d)) continue;
            seen.insert(d);
            devices.append({d, qint64(o.value("createdAt").toDouble())});
        }
        while (QLayoutItem *it = self->m_devices->takeAt(0)) {
            delete it->widget();
            delete it;
        }
        const QString here = self->m_session ? self->m_session->engine()->device() : QString();
        if (devices.isEmpty()) self->m_devices->addWidget(text(QStringLiteral("—"), self, 13.5, true));
        for (const auto &[name, at] : devices) {
            auto *line = new QWidget(self);
            auto *h = new QHBoxLayout(line);
            h->setContentsMargins(0, 0, 0, 0);
            h->setSpacing(8);
            const QString icon = name.startsWith(QLatin1String("desktop")) ? QStringLiteral("monitor") : name.startsWith(QLatin1String("web")) ? QStringLiteral("globe") : QStringLiteral("key");
            auto *glyph = new QLabel(line);
            glyph->setPixmap(icons::pixmap(icon, Theme::t().faint, 14, self->devicePixelRatioF()));
            h->addWidget(glyph);
            h->addWidget(text(deviceLabel(name), line), 0);
            if (name == here) {
                auto *badge = text(T("本机"), line, 11);
                badge->setContentsMargins(6, 1, 6, 1);
                QPalette bp = badge->palette();
                bp.setColor(QPalette::WindowText, Theme::t().ivyInk);
                bp.setColor(QPalette::Window, Theme::t().ivySoft);
                badge->setPalette(bp);
                badge->setAutoFillBackground(true);
                h->addWidget(badge);
            }
            h->addStretch(1);
            h->addWidget(text(when(at), line, 12, true));
            self->m_devices->addWidget(line);
        }
    });
}

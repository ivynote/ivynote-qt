#include <QRandomGenerator>
#include <QSignalBlocker>
#include "ui/historydialog.h"

#include "ui/markdownview.h"

#include "core/format.h"
#include "core/i18n.h"
#include "core/markdown.h"
#include "sync/api.h"
#include "sync/engine.h"
#include "ui/render.h"
#include "ui/theme.h"
#include "ui/toast.h"
#include "ui/widgets.h"
#include "vault/session.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QPushButton>
#include <QSplitter>
#include <QTabBar>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

HistoryDialog::HistoryDialog(Session *session, const QString &noteId, QWidget *parent)
    : QDialog(parent), m_session(session), m_noteId(noteId) {
    setWindowTitle(T("历史版本"));
    resize(860, 560);
    auto *col = new QVBoxLayout(this);
    auto *split = new QSplitter(this);
    m_list = new QListWidget(split);
    m_list->setMinimumWidth(220);
    auto *right = new QWidget(split);
    auto *rcol = new QVBoxLayout(right);
    rcol->setContentsMargins(0, 0, 0, 0);
    m_mode = new QTabBar(right);
    m_mode->addTab(T("渲染"));
    m_mode->addTab(T("源码"));
    m_mode->setDrawBase(false);
    rcol->addWidget(m_mode, 0, Qt::AlignLeft);
    m_view = new MarkdownView(right);
    if (session) {
        QPointer<Session> s(session);
        m_view->image = [s](const QString &id) { return s ? s->image(id, false) : QImage(); };
        m_view->cachedImage = [s](const QString &id) { return s ? s->cachedImage(id, false) : QImage(); };
        m_view->imageMeta = [s](const QString &id) -> QSize {
            if (!s) return {};
            const Attachment a = s->engine()->attachments().value(id);
            return a.width > 0 && a.height > 0 ? QSize(a.width, a.height) : QSize();
        };
        connect(session, &Session::attachmentReady, m_view, &MarkdownView::refreshAttachment);
    }
    rcol->addWidget(m_view, 1);
    split->addWidget(m_list);
    split->addWidget(right);
    split->setStretchFactor(1, 1);
    col->addWidget(split, 1);
    auto *row = new QHBoxLayout;
    m_status = mutedLabel(QString(), this, 12);
    row->addWidget(m_status, 1);
    m_restore = new QPushButton(T("恢复此版本"), this);
    m_restore->setEnabled(false);
    auto *close = new QPushButton(T("关闭"), this);
    row->addWidget(m_restore);
    row->addWidget(close);
    col->addLayout(row);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_list, &QListWidget::currentRowChanged, this, &HistoryDialog::showRevision);
    connect(m_mode, &QTabBar::currentChanged, this, &HistoryDialog::render);
    connect(m_restore, &QPushButton::clicked, this, [this] {
        if (!m_session) return;
        NotePatch p;
        p.title = m_title;
        p.body = m_body;
        m_session->engine()->updateNote(m_noteId, p);
        Toaster::show(T("已恢复，之前的内容仍保留在历史中"));
        accept();
    });
    load();
}

void HistoryDialog::load() {
    m_status->setText(T("正在加载…"));
    QPointer<HistoryDialog> self(this);
    m_session->api()->get(QStringLiteral("/notes/%1/history").arg(m_noteId), this, [self](const ApiReply &r) {
        if (!self) return;
        if (!r.ok) {
            self->m_status->setText(r.status == 404 ? T("这篇笔记还没有同步到服务器，暂无历史版本。")
                                    : r.transient() ? T("需要联网才能查看历史版本。")
                                                    : r.error);
            return;
        }
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const QJsonArray arr = r.json.toArray();
        for (int i = 0; i < arr.size(); ++i) {
            const QJsonObject o = arr[i].toObject();
            QString line = fmt::fullTime(qint64(o.value("createdAt").toDouble()), now);
            if (i == 0) line += QStringLiteral(" · ") + T("当前版本");
            const QString sub = QStringLiteral("%1 · %2").arg(o.value("device").toString(), fmt::bytes(qint64(o.value("size").toDouble())));
            auto *it = new QListWidgetItem(line + u'\n' + sub, self->m_list);
            it->setData(Qt::UserRole, qint64(o.value("rev").toDouble()));
        }
        self->m_status->setText(arr.isEmpty() ? T("暂无历史版本。") : QString());
        if (!arr.isEmpty()) self->m_list->setCurrentRow(0);
    });
}

void HistoryDialog::showRevision(int row) {
    if (row < 0 || !m_session) return;
    const qint64 rev = m_list->item(row)->data(Qt::UserRole).toLongLong();
    m_restore->setEnabled(false);
    QPointer<HistoryDialog> self(this);
    m_session->api()->get(QStringLiteral("/notes/%1/history/%2").arg(m_noteId).arg(rev), this, [self, row](const ApiReply &r) {
        if (!self || self->m_list->currentRow() != row) return;
        if (!r.ok) {
            self->m_status->setText(r.error);
            return;
        }
        self->m_title = r.json.toObject().value("title").toString();
        self->m_body = r.json.toObject().value("body").toString();
        self->m_restore->setEnabled(row > 0);
        self->render();
    });
}

void HistoryDialog::render() {
    if (m_mode->currentIndex() == 1) {
        m_view->setFont(Theme::mono(13));
        m_view->setPlainText(m_body);
    } else {
        m_view->setBaseSize(15);
        m_view->setFont(Theme::uiFont(15));
        RenderOptions opts;
        if (m_session) opts.resourceKind = [s = m_session](const QString &id) { return s ? s->engine()->attachmentKind(id) : QString(); };
        m_view->setMarkdown(m_body, opts, m_title.isEmpty() ? T("无标题") : m_title);
    }
}

// --- share ---

ShareDialog::ShareDialog(Session *session, const QString &noteId, QWidget *parent) : QDialog(parent), m_session(session), m_noteId(noteId) {
    setWindowTitle(T("分享"));
    setMinimumWidth(460);
    auto *col = new QVBoxLayout(this);
    col->setSpacing(12);
    m_enabled = new QCheckBox(T("通过公开链接分享"), this);
    col->addWidget(m_enabled);
    auto *form = new QFormLayout;
    auto *linkRow = new QHBoxLayout;
    m_link = new QLineEdit(this);
    m_link->setReadOnly(true);
    auto *copy = new QPushButton(T("复制链接"), this);
    linkRow->addWidget(m_link, 1);
    linkRow->addWidget(copy);
    form->addRow(T("链接"), linkRow);
    m_expiry = new QComboBox(this);
    m_expiry->addItems({T("永不过期"), T("1 天"), T("7 天"), T("30 天")});
    form->addRow(T("有效期"), m_expiry);
    auto *pwRow = new QHBoxLayout;
    m_password = new QLineEdit(this);
    m_password->setEchoMode(QLineEdit::Normal);
    m_password->setPlaceholderText(T("分享密码为 6–72 个字节"));
    auto *setPw = new QPushButton(T("设置"), this);
    auto *copyPw = new QPushButton(T("复制密码"), this);
    pwRow->addWidget(m_password, 1);
    pwRow->addWidget(setPw);
    pwRow->addWidget(copyPw);
    form->addRow(T("密码"), pwRow);
    m_info = mutedLabel(QString(), this, 12);
    form->addRow(QString(), m_info);
    col->addLayout(form);
    m_status = mutedLabel(QString(), this, 12);
    m_status->setWordWrap(true);
    col->addWidget(m_status);

    auto *local = new QHBoxLayout;
    auto *md = new QPushButton(T("复制 Markdown"), this);
    auto *rich = new QPushButton(T("复制富文本"), this);
    auto *file = new QPushButton(T("导出 .md"), this);
    local->addWidget(md);
    local->addWidget(rich);
    local->addWidget(file);
    local->addStretch(1);
    col->addLayout(local);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setText(T("关闭"));
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    col->addWidget(buttons);

    connect(copy, &QPushButton::clicked, this, [this] {
        if (m_link->text().isEmpty()) return;
        QApplication::clipboard()->setText(m_link->text());
        Toaster::show(T("链接已复制"));
    });
    connect(m_enabled, &QCheckBox::toggled, this, [this](bool on) {
        if (m_loading) return;
        QString pw = m_password->text();
        if (on && !m_hasPassword && pw.isEmpty()) {
            const QString alphabet = QStringLiteral("abcdefghjkmnpqrstuvwxyzABCDEFGHJKMNPQRSTUVWXYZ23456789");
            for (int i = 0; i < 8; ++i)
                pw += alphabet.at(QRandomGenerator::system()->bounded(int(alphabet.size())));
        }
        if (on && !m_hasPassword && (pw.toUtf8().size() < 6 || pw.toUtf8().size() > 72)) {
            Toaster::show(T("分享密码为 6–72 个字节"), true);
            QSignalBlocker blocker(m_enabled);
            m_enabled->setChecked(false);
            return;
        }
        put(on, m_expiresAt, on && !m_hasPassword ? &pw : nullptr);
    });
    connect(m_expiry, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (m_loading) return;
        static const int days[] = {0, 1, 7, 30};
        const qint64 at = days[i] ? QDateTime::currentMSecsSinceEpoch() + qint64(days[i]) * 86400000 : 0;
        put(m_enabled->isChecked(), at, nullptr);
    });
    connect(setPw, &QPushButton::clicked, this, [this] {
        const QString pw = m_password->text();
        if (pw.toUtf8().size() < 6 || pw.toUtf8().size() > 72) {
            Toaster::show(T("分享密码为 6–72 个字节"), true);
            return;
        }
        put(m_enabled->isChecked(), m_expiresAt, &pw);
    });
    connect(copyPw, &QPushButton::clicked, this, [this] {
        if (!m_password->text().isEmpty()) QApplication::clipboard()->setText(m_password->text());
    });
    connect(md, &QPushButton::clicked, this, &ShareDialog::copyMarkdown);
    connect(rich, &QPushButton::clicked, this, &ShareDialog::copyRich);
    connect(file, &QPushButton::clicked, this, &ShareDialog::exportFile);
    if (m_session->engine()->hasPending(EntityType::Note, noteId))
        m_status->setText(T("这篇笔记有尚未同步的修改，分享页会在同步后更新。"));
    load();
}

void ShareDialog::apply(const QJsonObject &s) {
    m_loading = true;
    const bool on = !s.isEmpty() && s.value("enabled").toBool();
    m_enabled->setChecked(on);
    m_expiresAt = qint64(s.value("expiresAt").toDouble());
    m_hasPassword = s.value("hasPassword").toBool();
    m_link->setText(s.isEmpty() ? QString() : m_session->info().server + QStringLiteral("/s/") + s.value("id").toString());
    if (m_expiresAt == 0) m_expiry->setCurrentIndex(0);
    m_password->setText(s.value("password").toString());
    m_password->setPlaceholderText(m_hasPassword ? T("已设置密码") : T("分享密码为 6–72 个字节"));
    QString info;
    if (!s.isEmpty()) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        info = (i18n::isZh() ? QStringLiteral("访问 %1 次") : QStringLiteral("%1 views")).arg(s.value("views").toInt());
        if (m_expiresAt) info += QStringLiteral(" · ") + T("过期时间") + u' ' + fmt::fullTime(m_expiresAt, now);
        if (s.value("expired").toBool()) info += QStringLiteral(" · ") + T("已过期");
    }
    m_info->setText(info);
    m_loading = false;
}

void ShareDialog::load() {
    QPointer<ShareDialog> self(this);
    m_session->api()->get(QStringLiteral("/notes/%1/share").arg(m_noteId), this, [self](const ApiReply &r) {
        if (!self) return;
        if (!r.ok) {
            self->m_enabled->setEnabled(false);
            self->m_status->setText(r.transient() ? T("离线中：只能复制或导出本机的文本。") : r.status == 404 ? T("这篇笔记还没有同步到服务器。") : r.error);
            return;
        }
        self->apply(r.json.toObject());
    });
}

void ShareDialog::put(bool enabled, qint64 expiresAt, const QString *password) {
    QJsonObject body{{"enabled", enabled}, {"expiresAt", double(expiresAt)}};
    if (password) body.insert(QStringLiteral("password"), *password);
    QPointer<ShareDialog> self(this);
    m_session->api()->putJson(QStringLiteral("/notes/%1/share").arg(m_noteId), body, this, [self](const ApiReply &r) {
        if (!self) return;
        if (!r.ok) {
            Toaster::show(r.transient() ? T("需要联网") : r.error, true);
            return self->load();
        }
        self->apply(r.json.toObject());
    });
}

void ShareDialog::copyMarkdown() {
    const Note *n = m_session->engine()->note(m_noteId);
    if (!n) return;
    QApplication::clipboard()->setText(n->body);
    Toaster::show(T("已复制 Markdown"));
}

void ShareDialog::copyRich() {
    const Note *n = m_session->engine()->note(m_noteId);
    if (!n) return;
    auto *mime = new QMimeData;
    mime->setHtml(QStringLiteral("<h1>%1</h1>").arg(n->title.toHtmlEscaped()) + renderMarkdown(n->body));
    mime->setText(n->body);
    QApplication::clipboard()->setMimeData(mime);
    Toaster::show(T("已复制富文本"));
}

void ShareDialog::exportFile() {
    const Note *n = m_session->engine()->note(m_noteId);
    if (!n) return;
    QString name = n->title.isEmpty() ? T("无标题") : n->title;
    name.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|])")), QStringLiteral("_"));
    const QString path = QFileDialog::getSaveFileName(this, T("导出 .md"), QDir::home().filePath(name + QStringLiteral(".md")), QStringLiteral("Markdown (*.md)"));
    if (path.isEmpty()) return;
    QString text = n->body;
    // Add the title as a heading unless the text already starts with it.
    const auto outline = md::outline(text);
    if (!n->title.isEmpty() && (outline.isEmpty() || outline.first().text != n->title)) text = QStringLiteral("# ") + n->title + QStringLiteral("\n\n") + text;
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(text.toUtf8());
        Toaster::show(T("已导出"));
    }
}

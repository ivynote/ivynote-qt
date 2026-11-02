#include "ui/notepane.h"

#include "core/format.h"
#include "core/i18n.h"
#include "core/markdown.h"
#include "core/models.h"
#include "sync/engine.h"
#include "ui/dialogs.h"
#include "ui/editor.h"
#include "ui/highlighter.h"
#include "ui/icons.h"
#include "ui/render.h"
#include "ui/theme.h"
#include "ui/toast.h"
#include "ui/widgets.h"
#include "vault/session.h"

#include <QAbstractTextDocumentLayout>
#include <QBuffer>
#include <QDateTime>
#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMimeDatabase>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSplitter>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextFrame>
#include <QTextTable>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidgetAction>

// A row of tag chips followed by the add button.
class FlowRow : public QWidget {
public:
    explicit FlowRow(QWidget *parent) : QWidget(parent), m_layout(new QHBoxLayout(this)) {
        m_layout->setContentsMargins(0, 0, 0, 0);
        m_layout->setSpacing(4);
    }
    void clear() {
        while (QLayoutItem *it = m_layout->takeAt(0)) {
            delete it->widget();
            delete it;
        }
    }
    void add(QWidget *w) { m_layout->addWidget(w); }
    void finish() { m_layout->addStretch(1); }

private:
    QHBoxLayout *m_layout;
};

namespace {

// Rendered preview that loads ivy://res images from the vault.
class Preview : public QTextBrowser {
public:
    explicit Preview(QWidget *parent) : QTextBrowser(parent) {
        setOpenLinks(false);
        setOpenExternalLinks(false);
        setFrameShape(QFrame::NoFrame);
        document()->setDocumentMargin(24);
    }
    std::function<QImage(const QString &id)> image;

protected:
    QVariant loadResource(int type, const QUrl &name) override {
        if (type == QTextDocument::ImageResource && name.scheme() == QLatin1String("ivy") && name.host() == QLatin1String("res")) {
            const QString id = name.path().mid(1);
            QImage img = image ? image(id) : QImage();
            if (img.isNull()) {
                img = QImage(320, 120, QImage::Format_ARGB32_Premultiplied);
                img.fill(Theme::t().sunken);
            }
            const int maxW = std::max(200, viewport()->width() - 60);
            if (img.width() > maxW) img = img.scaledToWidth(maxW, Qt::SmoothTransformation);
            return img;
        }
        // Nothing else is fetched: no remote images, no file access.
        return {};
    }
};

QString previewCss() {
    const Tokens &k = Theme::t();
    return QStringLiteral(
               "body{color:%1;} h1,h2,h3,h4{font-family:'%2';color:%1;font-weight:600;}"
               "h1{font-size:1.6em;} h2{font-size:1.3em;} h3{font-size:1.1em;}"
               "a{color:%3;text-decoration:none;} a.missing{color:%4;}"
               "code{font-family:'%5';background-color:%6;} pre{font-family:'%5';background-color:%6;padding:10px;}"
               "blockquote{color:%7;font-family:'%2';margin-left:12px;padding-left:12px;border-left:2px solid %3;}"
               "table{border-collapse:collapse;margin-top:6px;margin-bottom:6px;} td,th{padding:5px 12px;border:1px solid %8;}"
               "th{background-color:%10;font-weight:600;}"
               ".mark{background-color:%9;} .task{color:%3;font-size:1.1em;} .file{color:%3;}"
               "hr{color:%8;}")
        .arg(k.ink.name(), Theme::serif(16).families().value(0, QStringLiteral("serif")), k.ivy.name(), k.muted.name(),
             Theme::mono(13).families().value(0, QStringLiteral("monospace")), k.codeBg.name(), k.ink2.name(), k.line.name(),
             Theme::alpha(k.warn, 0.25).name(QColor::HexArgb), k.sunken.name());
}

} // namespace

NotePane::NotePane(QWidget *parent) : QWidget(parent) {
    auto *col = new QVBoxLayout(this);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);

    // Top bar: breadcrumb, status, actions.
    m_topBar = new QWidget(this);
    m_topBar->setFixedHeight(48);
    auto *top = new QHBoxLayout(m_topBar);
    top->setContentsMargins(14, 8, 12, 0);
    top->setSpacing(4);
    m_expandShelf = new IconButton(QStringLiteral("sidebar"), T("展开书架") + QStringLiteral(" (Ctrl+\\)"), m_topBar);
    m_expandShelf->hide();
    connect(m_expandShelf, &QToolButton::clicked, this, &NotePane::expandShelfRequested);
    top->addWidget(m_expandShelf);
    auto *crumbHost = new QWidget(m_topBar);
    m_crumbs = new QHBoxLayout(crumbHost);
    m_crumbs->setContentsMargins(4, 0, 0, 0);
    m_crumbs->setSpacing(2);
    top->addWidget(crumbHost);
    m_pendingDot = new QLabel(m_topBar);
    m_pendingDot->setFixedSize(14, 14);
    m_pendingDot->setToolTip(T("有待同步的修改"));
    top->addWidget(m_pendingDot);
    m_savedLabel = mutedLabel(T("已保存到本机"), m_topBar, 12);
    m_savedLabel->hide();
    top->addWidget(m_savedLabel);
    top->addStretch(1);
    m_shareButton = new IconButton(QStringLiteral("share"), T("分享"), m_topBar);
    m_outlineButton = new IconButton(QStringLiteral("outline"), T("大纲"), m_topBar);
    m_outlineButton->setCheckable(true);
    m_moreButton = new IconButton(QStringLiteral("more"), T("更多"), m_topBar);
    connect(m_shareButton, &QToolButton::clicked, this, [this] { emit shareRequested(m_id); });
    connect(m_outlineButton, &QToolButton::clicked, this, &NotePane::toggleOutline);
    connect(m_moreButton, &QToolButton::clicked, this, &NotePane::showMoreMenu);
    top->addWidget(m_shareButton);
    top->addWidget(m_outlineButton);
    top->addWidget(m_moreButton);
    col->addWidget(m_topBar);

    // Trash banner.
    m_banner = new QWidget(this);
    auto *bannerRow = new QHBoxLayout(m_banner);
    bannerRow->setContentsMargins(24, 8, 24, 8);
    auto *bannerText = new QLabel(T("这篇笔记在回收站中，只读。"), m_banner);
    bannerRow->addWidget(bannerText, 1);
    auto *restore = new PillButton(T("恢复"), PillButton::Secondary, m_banner);
    auto *purge = new PillButton(T("永久删除"), PillButton::Danger, m_banner);
    bannerRow->addWidget(restore);
    bannerRow->addWidget(purge);
    connect(restore, &QToolButton::clicked, this, [this] {
        NotePatch p;
        p.trashed = false;
        if (m_engine) m_engine->updateNote(m_id, p);
    });
    connect(purge, &QToolButton::clicked, this, [this] {
        if (m_engine && dialogs::confirm(this, T("永久删除"), T("永久删除这篇笔记？此操作无法撤销。"), T("永久删除"))) {
            const QString id = m_id;
            setNote(QString());
            m_engine->deleteNote(id);
        }
    });
    m_banner->setAutoFillBackground(true);
    m_banner->hide();
    col->addWidget(m_banner);

    // Title and meta, on the same column as the text.
    m_headHost = new QWidget(this);
    auto *head = m_head = new QWidget(m_headHost);
    auto *headCol = new QVBoxLayout(head);
    headCol->setContentsMargins(0, 18, 0, 6);
    headCol->setSpacing(6);
    m_title = new QLineEdit(head);
    m_title->setFrame(false);
    // Native styles pad line edits differently (Breeze adds several pixels);
    // without padding the title starts exactly where the text does.
    m_title->setStyleSheet(QStringLiteral("QLineEdit{border:0;padding:0;margin:0;background:transparent;}"));
    m_title->setTextMargins(0, 0, 0, 0);
    m_title->setFont(Theme::serif(30));
    m_title->setPlaceholderText(T("无标题"));
    m_title->setMaxLength(300);
    m_title->setAttribute(Qt::WA_MacShowFocusRect, false);
    passAppShortcuts(m_title);
    headCol->addWidget(m_title);
    auto *metaRow = new QWidget(head);
    auto *metaLayout = new QHBoxLayout(metaRow);
    metaLayout->setContentsMargins(2, 0, 0, 0);
    metaLayout->setSpacing(10);
    m_meta = mutedLabel(QString(), metaRow, 12.5);
    metaLayout->addWidget(m_meta);
    m_tagsRow = new FlowRow(metaRow);
    metaLayout->addWidget(m_tagsRow, 1);
    headCol->addWidget(metaRow);
    auto *headRow = new QHBoxLayout(m_headHost);
    headRow->setContentsMargins(24, 0, 24, 0);
    headRow->addWidget(head);
    col->addWidget(m_headHost);

    // Editor, preview and outline.
    auto *body = m_body = new QWidget(this);
    auto *bodyRow = new QHBoxLayout(body);
    bodyRow->setContentsMargins(0, 0, 0, 0);
    bodyRow->setSpacing(0);
    m_split = new Divider(Qt::Horizontal, body);
    m_split->setChildrenCollapsible(false);
    m_editor = new MarkdownEditor(m_split);
    passAppShortcuts(m_editor);
    m_preview = new Preview(m_split);
    m_split->addWidget(m_editor);
    m_split->addWidget(m_preview);
    m_preview->hide();
    bodyRow->addWidget(m_split, 1);
    m_outline = new QListWidget(body);
    m_outline->setFixedWidth(224);
    m_outline->setFrameShape(QFrame::NoFrame);
    m_outline->setFont(Theme::uiFont(13));
    m_outline->hide();
    bodyRow->addWidget(m_outline);
    col->addWidget(body, 1);

    // Backlinks.
    m_backlinksButton = new QToolButton(this);
    m_backlinksButton->setAutoRaise(true);
    m_backlinksButton->setCursor(Qt::PointingHandCursor);
    m_backlinksButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_backlinksButton->setFont(Theme::uiFont(12.5));
    m_backlinksButton->hide();
    m_backlinks = new QListWidget(this);
    m_backlinks->setFrameShape(QFrame::NoFrame);
    m_backlinks->setMaximumHeight(160);
    m_backlinks->hide();
    auto *blRow = new QHBoxLayout;
    blRow->setContentsMargins(24, 2, 24, 8);
    blRow->addWidget(m_backlinksButton);
    blRow->addStretch(1);
    col->addLayout(blRow);
    col->addWidget(m_backlinks);
    connect(m_backlinksButton, &QToolButton::clicked, this, [this] {
        if (m_backlinks->isVisible()) {
            slide(m_backlinks, Qt::Vertical, m_backlinks->height(), 0, 160, [this] { m_backlinks->hide(); m_backlinks->setMaximumHeight(160); });
        } else {
            m_backlinks->show();
            slide(m_backlinks, Qt::Vertical, 0, 160, 200, [this] { m_backlinks->setMaximumHeight(160); });
        }
    });
    connect(m_backlinks, &QListWidget::itemActivated, this, [this](QListWidgetItem *it) { emit openNote(it->data(Qt::UserRole).toString()); });
    connect(m_backlinks, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) { emit openNote(it->data(Qt::UserRole).toString()); });
    connect(m_outline, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) {
        const int line = it->data(Qt::UserRole).toInt();
        if (m_mode == EditorMode::Split) m_preview->scrollToAnchor(it->data(Qt::UserRole + 1).toString());
        m_editor->scrollToLine(line);
    });

    // Saving: edits reach the engine shortly after typing pauses.
    m_bodyTimer.setSingleShot(true);
    m_bodyTimer.setInterval(300);
    m_titleTimer.setSingleShot(true);
    m_titleTimer.setInterval(300);
    m_previewTimer.setSingleShot(true);
    m_previewTimer.setInterval(120);
    m_indexTimer.setSingleShot(true);
    m_indexTimer.setInterval(800);
    m_savedTimer.setSingleShot(true);
    m_savedTimer.setInterval(1600);
    connect(&m_bodyTimer, &QTimer::timeout, this, &NotePane::flush);
    connect(&m_titleTimer, &QTimer::timeout, this, &NotePane::flush);
    connect(&m_previewTimer, &QTimer::timeout, this, &NotePane::refreshPreview);
    connect(&m_indexTimer, &QTimer::timeout, this, &NotePane::refreshIndex);
    connect(&m_savedTimer, &QTimer::timeout, this, [this] { sink(m_savedLabel, 400); });
    connect(m_editor, &MarkdownEditor::columnChanged, this, &NotePane::alignHead);
    // Split view: each side follows the other.
    connect(m_editor->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { syncScroll(true); });
    connect(m_preview->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { syncScroll(false); });
    connect(m_editor->document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this, [this] { m_scrollMap.clear(); });
    connect(m_preview->document(), &QTextDocument::documentLayoutChanged, this, [this] {
        m_scrollMap.clear();
        connect(m_preview->document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this, [this] { m_scrollMap.clear(); },
                Qt::UniqueConnection);
    });
    connect(m_preview->document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this, [this] { m_scrollMap.clear(); });
    connect(m_editor, &MarkdownEditor::edited, this, [this] {
        m_bodyDirty = true;
        m_bodyTimer.start();
        if (m_mode == EditorMode::Split) m_previewTimer.start();
    });
    connect(m_title, &QLineEdit::textEdited, this, [this] {
        m_titleDirty = true;
        m_titleTimer.start();
    });
    connect(m_title, &QLineEdit::returnPressed, this, [this] {
        // Enter in the title goes to the start of the text.
        flush();
        QTextCursor c = m_editor->textCursor();
        c.movePosition(QTextCursor::Start);
        m_editor->setTextCursor(c);
        m_editor->setFocus();
    });
    connect(m_editor, &MarkdownEditor::linkActivated, this, &NotePane::openLink);
    connect(m_editor, &MarkdownEditor::filesDropped, this, &NotePane::insertFiles);
    connect(m_editor, &MarkdownEditor::imagePasted, this, &NotePane::addImage);
    connect(m_preview, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) {
        const QString s = url.toString();
        if (s.startsWith(QLatin1String("ivy-task:"))) {
            if (m_engine && !m_engine->note(m_id)->trashed()) {
                flush();
                const QString body = md::toggleTask(m_editor->markdown(), s.mid(9).toInt());
                m_editor->applyRemote(body);
                flush();
            }
            return;
        }
        if (s.startsWith(QLatin1String("ivy-wiki:"))) return openLink(QUrl::fromPercentEncoding(s.mid(9).toUtf8()), true);
        openLink(s, false);
    });
    auto restyle = [this] {
        const Tokens &k = Theme::t();
        QPalette p = palette();
        p.setColor(QPalette::Window, k.surface);
        p.setColor(QPalette::Base, k.surface);
        setPalette(p);
        setAutoFillBackground(false);
        QPalette bp = m_banner->palette();
        bp.setColor(QPalette::Window, k.warnSoft);
        bp.setColor(QPalette::WindowText, k.warnInk);
        m_banner->setPalette(bp);
        QPalette tp = m_title->palette();
        tp.setColor(QPalette::Base, k.surface);
        tp.setColor(QPalette::Text, k.ink);
        m_title->setPalette(tp);
        for (QWidget *w : {static_cast<QWidget *>(m_outline), static_cast<QWidget *>(m_backlinks), static_cast<QWidget *>(m_preview)}) {
            QPalette lp = w->palette();
            lp.setColor(QPalette::Base, k.surface);
            lp.setColor(QPalette::Text, k.ink2);
            w->setPalette(lp);
        }
        m_preview->document()->setDefaultStyleSheet(previewCss());
        m_backlinksButton->setIcon(icons::icon(QStringLiteral("backlink"), k.muted));
        refresh();
        refreshPreview();
    };
    connect(Theme::instance(), &Theme::changed, this, restyle);
    restyle();
    setNote(QString());
}

void NotePane::setSession(Session *session) {
    flush();
    if (m_engine) m_engine->disconnect(this);
    if (m_session) m_session->disconnect(this);
    m_session = session;
    m_engine = session ? session->engine() : nullptr;
    m_id.clear();
    if (!session) return setNote(QString());
    m_shareButton->setVisible(session->isRemote());
    connect(m_engine, &SyncEngine::aboutToApplyRemote, this, &NotePane::flush, Qt::DirectConnection);
    connect(m_engine, &SyncEngine::remoteBody, this, [this](const QString &id, const QString &body) {
        if (id == m_id && !m_bodyDirty && m_editor->markdown() != body) m_editor->applyRemote(body);
    });
    connect(m_engine, &SyncEngine::dataChanged, this, [this] {
        refresh();
        m_indexTimer.start();
    });
    connect(m_engine, &SyncEngine::stateChanged, this, &NotePane::refresh);
    connect(session, &Session::attachmentReady, this, [this](const QString &id) {
        m_editor->refreshAttachment(id);
        if (m_mode == EditorMode::Split) m_previewTimer.start();
    });
    QPointer<Session> s(session);
    m_editor->image = [s](const QString &id) -> QImage {
        if (!s) return {};
        const Attachment a = s->engine()->attachments().value(id);
        return s->image(id, s->isRemote() && a.hasThumb);
    };
    m_editor->imageMeta = [s](const QString &id) -> QSize {
        if (!s) return {};
        const Attachment a = s->engine()->attachments().value(id);
        if (a.width <= 0 || a.height <= 0) return {};
        QSize size(a.width, a.height);
        if (s->isRemote() && a.hasThumb && std::max(a.width, a.height) > 480) size = size.scaled(480, 480, Qt::KeepAspectRatio);
        return size;
    };
    m_editor->attachmentKind = [s](const QString &id) { return s ? s->engine()->attachmentKind(id) : QString(); };
    m_editor->wikiExists = [this](const QString &t) { return !resolveTitle(t).isEmpty(); };
    static_cast<Preview *>(m_preview)->image = [s](const QString &id) { return s ? s->image(id, false) : QImage(); };
    refreshIndex();
    setNote(QString());
}

void NotePane::setNote(const QString &id) {
    flush();
    m_id = id;
    m_bodyDirty = m_titleDirty = false;
    const Note *n = m_engine && !id.isEmpty() ? m_engine->note(id) : nullptr;
    if (!n) m_id.clear();
    const bool has = n != nullptr;
    for (QWidget *w : findChildren<QWidget *>(Qt::FindDirectChildrenOnly)) w->setEnabled(has);
    m_title->setText(n ? n->title : QString());
    m_editor->setMarkdown(n ? n->body : QString());
    m_editor->setReadOnly(!n || n->trashed());
    m_title->setReadOnly(!n || n->trashed());
    m_backlinks->hide();
    refresh();
    refreshIndex();
    refreshOutline();
    if (m_mode == EditorMode::Split) refreshPreview();
    if (has) {
        // Note to note: a quick cross-fade, not a cut.
        rise(m_headHost, 180);
        rise(m_body, 180);
    }
}

void NotePane::flush() {
    if (!m_engine || m_id.isEmpty()) return;
    m_bodyTimer.stop();
    m_titleTimer.stop();
    if (!m_bodyDirty && !m_titleDirty) return;
    const Note *n = m_engine->note(m_id);
    if (!n || n->trashed()) {
        m_bodyDirty = m_titleDirty = false;
        return;
    }
    NotePatch p;
    if (m_bodyDirty) p.body = m_editor->markdown();
    if (m_titleDirty) p.title = m_title->text().simplified().left(300);
    m_bodyDirty = m_titleDirty = false;
    m_engine->updateNote(m_id, p);
    if (!m_savedLabel->isVisible()) {
        m_savedLabel->show();
        rise(m_savedLabel, 200);
    }
    m_savedTimer.start();
    if (p.body) refreshOutline();
}

void NotePane::refresh() {
    const Tokens &k = Theme::t();
    const Note *n = m_engine && !m_id.isEmpty() ? m_engine->note(m_id) : nullptr;
    if (!n && !m_id.isEmpty()) {
        // Deleted here or by another device.
        m_id.clear();
        setNote(QString());
        return;
    }
    // Breadcrumb.
    while (QLayoutItem *it = m_crumbs->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    if (n) {
        auto crumb = [this, &k](const QString &text, const QString &folderId, bool last) {
            auto *b = new QToolButton;
            b->setAutoRaise(true);
            b->setText(text);
            b->setFont(Theme::uiFont(13));
            b->setCursor(Qt::PointingHandCursor);
            b->setStyleSheet(QStringLiteral("QToolButton{border:none;padding:2px 4px;color:%1;}QToolButton:hover{color:%2;}")
                                 .arg((last ? k.muted : k.faint).name(), k.ink2.name()));
            connect(b, &QToolButton::clicked, this, [this, folderId] { emit revealFolder(folderId); });
            m_crumbs->addWidget(b);
        };
        if (n->trashed()) {
            crumb(T("回收站"), QStringLiteral("v:trash"), true);
        } else {
            const auto path = m_engine->folderPath(n->folderId);
            if (path.isEmpty()) crumb(T("笔记"), QString(), true);
            for (int i = 0; i < path.size(); ++i) {
                if (i) {
                    auto *sep = new QLabel(QStringLiteral("/"));
                    sep->setStyleSheet(QStringLiteral("color:%1;").arg(k.faint.name()));
                    m_crumbs->addWidget(sep);
                }
                crumb(path[i]->name, path[i]->id, i == path.size() - 1);
            }
        }
    }
    // Pending dot.
    const bool pending = n && m_engine->hasPending(EntityType::Note, n->id) && m_session && m_session->isRemote();
    QPixmap dot(QSize(14, 14) * devicePixelRatioF());
    dot.setDevicePixelRatio(devicePixelRatioF());
    dot.fill(Qt::transparent);
    if (pending) {
        QPainter p(&dot);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(k.warn);
        p.drawEllipse(QPointF(7, 7), 3, 3);
    }
    m_pendingDot->setPixmap(dot);
    m_banner->setVisible(n && n->trashed());
    if (!n) {
        m_meta->clear();
        m_tagsRow->clear();
        return;
    }
    // Keep the title in step with other devices unless it is being typed.
    if (!m_titleDirty && !m_title->hasFocus() && m_title->text() != n->title) m_title->setText(n->title);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const int words = n->words > 0 ? n->words : md::wordCount(n->body);
    m_meta->setText(QStringLiteral("%1   %2 %3").arg(fmt::fullTime(n->updatedAt, now), fmt::number(words), T("字")));
    m_meta->setToolTip(T("创建于") + u' ' + fmt::fullTime(n->createdAt, now));
    // Tag chips.
    m_tagsRow->clear();
    for (const QString &tid : n->tags) {
        auto it = m_engine->tags().constFind(tid);
        if (it == m_engine->tags().constEnd()) continue;
        auto *chip = new QToolButton;
        chip->setAutoRaise(true);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setFont(Theme::uiFont(12.5));
        const QColor hash = Theme::tagColor(it->name, it->color);
        chip->setText(QStringLiteral("#") + it->name);
        chip->setStyleSheet(QStringLiteral("QToolButton{border:none;border-radius:4px;padding:2px 6px;color:%1;background:transparent;}"
                                           "QToolButton:hover{background:%2;}")
                                .arg(k.ink2.name(), k.sunken.name()));
        chip->setToolTip(T("点击移除"));
        const QString noteId = n->id;
        connect(chip, &QToolButton::clicked, this, [this, noteId, tid] {
            const Note *x = m_engine->note(noteId);
            if (!x || x->trashed()) return;
            NotePatch p;
            QStringList tags = x->tags;
            tags.removeAll(tid);
            p.tags = tags;
            m_engine->updateNote(noteId, p);
        });
        Q_UNUSED(hash);
        m_tagsRow->add(chip);
    }
    if (!n->trashed()) {
        auto *add = new QToolButton;
        add->setAutoRaise(true);
        add->setCursor(Qt::PointingHandCursor);
        add->setFont(Theme::uiFont(12.5));
        add->setText(QStringLiteral("+ ") + T("标签"));
        add->setStyleSheet(QStringLiteral("QToolButton{border:none;border-radius:4px;padding:2px 6px;color:%1;}QToolButton:hover{color:%2;background:%3;}")
                               .arg(k.faint.name(), k.ink2.name(), k.sunken.name()));
        connect(add, &QToolButton::clicked, this, &NotePane::pickTags);
        m_tagsRow->add(add);
    }
    m_tagsRow->finish();
    if (m_uploading > 0) {
        m_savedLabel->setText(T("正在上传…"));
        m_savedLabel->show();
    } else {
        m_savedLabel->setText(T("已保存到本机"));
    }
}

QString NotePane::resolveTitle(const QString &title) const { return m_titleIndex.value(md::normalizeTitle(title)); }

void NotePane::refreshIndex() {
    if (!m_engine) return;
    // Titles: newest wins on duplicates; trash excluded.
    QHash<QString, QPair<QString, qint64>> best;
    QStringList titles;
    for (const auto &n : m_engine->notes()) {
        if (n.trashed() || n.title.trimmed().isEmpty()) continue;
        const QString key = md::normalizeTitle(n.title);
        auto it = best.find(key);
        if (it == best.end() || it->second < n.updatedAt) best[key] = {n.id, n.updatedAt};
        if (n.id != m_id) titles << n.title;
    }
    const bool changed = best.size() != m_titleIndex.size();
    m_titleIndex.clear();
    for (auto it = best.begin(); it != best.end(); ++it) m_titleIndex.insert(it.key(), it->first);
    titles.removeDuplicates();
    titles.sort(Qt::CaseInsensitive);
    m_editor->setWikiTitles(titles);
    Q_UNUSED(changed);
    // Backlinks of the open note.
    m_backlinks->clear();
    const Note *cur = m_id.isEmpty() ? nullptr : m_engine->note(m_id);
    if (!cur || cur->trashed()) {
        m_backlinksButton->hide();
        m_backlinks->hide();
        return;
    }
    const QString key = md::normalizeTitle(cur->title);
    const bool isTarget = !key.isEmpty() && m_titleIndex.value(key) == cur->id;
    QList<const Note *> linking;
    for (const auto &n : m_engine->notes()) {
        if (n.id == cur->id || n.trashed()) continue;
        bool hit = false;
        if (isTarget && n.body.contains(QStringLiteral("[["))) {
            for (const QString &t : md::wikiTargets(n.body))
                if (t == key) hit = true;
        }
        if (!hit && n.body.contains(cur->id)) hit = md::noteRefs(n.body).contains(cur->id);
        if (hit) linking << &n;
    }
    std::sort(linking.begin(), linking.end(), [](const Note *a, const Note *b) { return a->updatedAt > b->updatedAt; });
    for (const Note *n : linking) {
        const QString ctx = md::linkContext(n->body, cur->title);
        auto *it = new QListWidgetItem((n->title.isEmpty() ? T("无标题") : n->title) + (ctx.isEmpty() ? QString() : QStringLiteral("  —  ") + ctx), m_backlinks);
        it->setData(Qt::UserRole, n->id);
        it->setIcon(icons::icon(QStringLiteral("file"), Theme::t().muted));
    }
    m_backlinksButton->setVisible(!linking.isEmpty());
    m_backlinksButton->setText((i18n::isZh() ? QStringLiteral("%1 条反向链接") : QStringLiteral("%1 backlinks")).arg(linking.size()));
    if (linking.isEmpty()) m_backlinks->hide();
}

void NotePane::refreshOutline() {
    m_outline->clear();
    if (!m_outlineOpen) return;
    for (const md::Heading &h : md::outline(m_editor->markdown())) {
        auto *it = new QListWidgetItem(QString((h.level - 1) * 2, u' ') + h.text, m_outline);
        it->setData(Qt::UserRole, h.line);
        it->setData(Qt::UserRole + 1, md::slug(h.text));
        it->setForeground(h.level <= 2 ? Theme::t().ink2 : Theme::t().muted);
    }
    if (m_outline->count() == 0) {
        auto *it = new QListWidgetItem(T("没有标题"), m_outline);
        it->setFlags(Qt::NoItemFlags);
    }
}

void NotePane::refreshPreview() {
    if (m_mode != EditorMode::Split || !m_session) return;
    RenderOptions opts;
    opts.resolveTitle = [this](const QString &t) { return resolveTitle(t); };
    opts.resourceKind = [this](const QString &id) { return m_engine->attachmentKind(id); };
    opts.resourceName = [this](const QString &id) { return m_engine->attachmentName(id); };
    opts.interactiveTasks = !m_editor->isReadOnly();
    const int scroll = m_preview->verticalScrollBar()->value();
    m_syncing = true;
    m_preview->setHtml(renderMarkdown(m_editor->markdown(), opts));
    m_preview->verticalScrollBar()->setValue(scroll);
    m_syncing = false;
    m_scrollMap.clear();
    syncScroll(true);
}

void NotePane::alignHead() {
    // The head spans the editor's text column, wherever the editor sits
    // (narrower in the split view, beside the outline).
    const QRect col = m_editor->textColumn();
    const QPoint at = m_editor->mapTo(this, col.topLeft());
    const int left = std::max(0, at.x());
    const int right = std::max(0, width() - left - col.width());
    if (auto *l = m_headHost->layout(); l->contentsMargins().left() != left || l->contentsMargins().right() != right)
        l->setContentsMargins(left, 0, right, 0);
}

void NotePane::buildScrollMap() {
    // Landmarks both sides share — headings, images, tables, in order — tie
    // the two scroll positions together; between them, interpolate.
    m_scrollMap.clear();
    QTextDocument *src = m_editor->document();
    QTextDocument *dst = m_preview->document();
    QAbstractTextDocumentLayout *sl = src->documentLayout();
    QAbstractTextDocumentLayout *dl = dst->documentLayout();
    QList<qreal> srcHeads, srcTables, dstHeads, dstTables;
    QList<QPair<QString, qreal>> srcImages, dstImages;
    static const QRegularExpression image(QStringLiteral(R"(!\[[^\]\n]*\]\(ivy://res/([A-Za-z0-9_-]{8,64})\))"));
    for (QTextBlock b = src->begin(); b.isValid(); b = b.next()) {
        const auto *info = static_cast<BlockInfo *>(b.userData());
        if (info && (info->kind == BlockInfo::Code || info->kind == BlockInfo::Fence)) continue;
        const qreal y = sl->blockBoundingRect(b).top();
        if (info && info->kind == BlockInfo::Heading) srcHeads << y;
        else if (info && info->kind == BlockInfo::Table && info->tableRow == 0) srcTables << y;
        if (b.text().contains(QLatin1String("ivy://res/")))
            for (auto it = image.globalMatch(b.text()); it.hasNext();) srcImages.append({it.next().captured(1), y});
    }
    QSet<const QTextFrame *> seen;
    for (QTextBlock b = dst->begin(); b.isValid(); b = b.next()) {
        const qreal y = dl->blockBoundingRect(b).top();
        if (b.blockFormat().headingLevel() > 0) dstHeads << y;
        if (QTextFrame *f = QTextCursor(b).currentFrame(); f && f != dst->rootFrame() && qobject_cast<QTextTable *>(f) && !seen.contains(f)) {
            seen.insert(f);
            dstTables << dl->frameBoundingRect(f).top();
        }
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextCharFormat cf = it.fragment().charFormat();
            if (!cf.isImageFormat()) continue;
            const QString name = cf.toImageFormat().name();
            if (name.startsWith(QLatin1String("ivy://res/"))) dstImages.append({name.mid(10), y});
        }
    }
    QList<QPair<qreal, qreal>> points;
    for (qsizetype i = 0; i < std::min(srcHeads.size(), dstHeads.size()); ++i) points.append({srcHeads[i], dstHeads[i]});
    for (qsizetype i = 0; i < std::min(srcTables.size(), dstTables.size()); ++i) points.append({srcTables[i], dstTables[i]});
    QHash<QString, int> used;
    for (const auto &[id, y] : std::as_const(srcImages)) {
        int n = used.value(id);
        for (const auto &[did, dy] : std::as_const(dstImages))
            if (did == id && n-- == 0) {
                points.append({y, dy});
                break;
            }
        used[id]++;
    }
    std::sort(points.begin(), points.end());
    const qreal srcMax = m_editor->verticalScrollBar()->maximum();
    const qreal dstMax = m_preview->verticalScrollBar()->maximum();
    m_scrollMap.append({0, 0});
    for (const auto &pt : std::as_const(points)) {
        // Keep it strictly increasing on both sides; drop landmarks out of order.
        if (pt.first <= m_scrollMap.last().first || pt.second <= m_scrollMap.last().second) continue;
        if (pt.first >= srcMax || pt.second >= dstMax) continue;
        m_scrollMap.append(pt);
    }
    m_scrollMap.append({std::max<qreal>(srcMax, m_scrollMap.last().first + 1), std::max<qreal>(dstMax, m_scrollMap.last().second + 1)});
}

void NotePane::syncScroll(bool fromEditor) {
    if (m_syncing || m_mode != EditorMode::Split || !m_preview->isVisible()) return;
    if (m_scrollMap.isEmpty()) buildScrollMap();
    QScrollBar *from = fromEditor ? m_editor->verticalScrollBar() : m_preview->verticalScrollBar();
    QScrollBar *to = fromEditor ? m_preview->verticalScrollBar() : m_editor->verticalScrollBar();
    const qreal v = from->value();
    qreal out = to->maximum();
    for (qsizetype i = 1; i < m_scrollMap.size(); ++i) {
        const qreal a = fromEditor ? m_scrollMap[i - 1].first : m_scrollMap[i - 1].second;
        const qreal b = fromEditor ? m_scrollMap[i].first : m_scrollMap[i].second;
        if (v > b && i + 1 < m_scrollMap.size()) continue;
        const qreal a2 = fromEditor ? m_scrollMap[i - 1].second : m_scrollMap[i - 1].first;
        const qreal b2 = fromEditor ? m_scrollMap[i].second : m_scrollMap[i].first;
        const qreal t = b > a ? std::clamp((v - a) / (b - a), 0.0, 1.0) : 0.0;
        out = a2 + t * (b2 - a2);
        break;
    }
    m_syncing = true;
    to->setValue(int(std::round(out)));
    m_syncing = false;
}

void NotePane::setEditorMode(EditorMode mode) {
    flush();
    m_mode = mode;
    m_editor->setMode(mode == EditorMode::Live ? MarkdownHighlighter::Live : MarkdownHighlighter::Source);
    const bool wasSplit = m_preview->isVisible();
    m_preview->setVisible(mode == EditorMode::Split);
    m_editor->setColumnWidth(mode == EditorMode::Split ? 560 : 680);
    if (mode == EditorMode::Split) {
        m_split->setSizes({1, 1});
        refreshPreview();
        if (!wasSplit) rise(m_preview, 220);
    }
    if (!m_id.isEmpty()) rise(m_editor, 160);
    emit editorModeChanged(mode);
}

void NotePane::setFontSize(int px) {
    m_editor->setFontSize(px);
    m_preview->setFont(Theme::uiFont(px));
}

void NotePane::setShelfCollapsed(bool collapsed) {
    m_shelfCollapsed = collapsed;
    m_expandShelf->setVisible(collapsed);
}

void NotePane::toggleOutline() {
    m_outlineOpen = !m_outlineOpen;
    m_outlineButton->setChecked(m_outlineOpen);
    refreshOutline();
    // The outline slides in from the right edge.
    if (m_outlineOpen) {
        m_outline->setMaximumWidth(0);
        m_outline->show();
        slide(m_outline, Qt::Horizontal, 0, 224, 200, [this] { m_outline->setFixedWidth(224); });
    } else {
        m_outline->setMinimumWidth(0);
        slide(m_outline, Qt::Horizontal, m_outline->width(), 0, 160, [this] {
            m_outline->hide();
            m_outline->setFixedWidth(224);
        });
    }
}

void NotePane::focusEditor() { m_editor->setFocus(); }
void NotePane::focusTitle() { m_title->setFocus(); }

void NotePane::openLink(const QString &target, bool wiki) {
    if (!m_engine) return;
    if (wiki) {
        const md::WikiLink w = md::parseWiki(target);
        if (w.target.isEmpty()) return;
        flush();
        QString id = resolveTitle(w.target);
        if (id.isEmpty()) {
            // A missing [[Title]] creates the note next to this one.
            const Note *cur = m_engine->note(m_id);
            NoteInit init;
            init.title = w.target.left(300);
            init.folderId = cur ? cur->folderId : QString();
            id = m_engine->createNote(init).id;
            refreshIndex();
        }
        emit openNote(id);
        if (!w.heading.isEmpty()) {
            const QString slug = md::slug(w.heading);
            for (const md::Heading &h : md::outline(m_editor->markdown()))
                if (md::slug(h.text) == slug) {
                    m_editor->scrollToLine(h.line);
                    break;
                }
        }
        return;
    }
    static const QRegularExpression note(QStringLiteral(R"(^ivy://note/([A-Za-z0-9_-]{8,64}))"));
    static const QRegularExpression res(QStringLiteral(R"(^ivy://res/([A-Za-z0-9_-]{8,64}))"));
    if (auto m = note.match(target); m.hasMatch()) {
        if (m_engine->note(m.captured(1))) emit openNote(m.captured(1));
        else Toaster::show(T("链接的笔记不存在"), true);
        return;
    }
    if (auto m = res.match(target); m.hasMatch()) {
        m_session->openAttachment(m.captured(1));
        return;
    }
    if (isSafeExternalUrl(target)) {
        QDesktopServices::openUrl(QUrl(target));
        return;
    }
    if (target.startsWith(u'#')) {
        const QString slug = target.mid(1);
        for (const md::Heading &h : md::outline(m_editor->markdown()))
            if (md::slug(h.text) == slug) m_editor->scrollToLine(h.line);
    }
    // Anything else (javascript:, file:, …) is ignored on purpose.
}

void NotePane::attach(const QByteArray &data, const QString &name, const QString &mime) {
    if (!m_engine || m_id.isEmpty()) return;
    const QString noteId = m_id;
    m_uploading++;
    refresh();
    QPointer<NotePane> self(this);
    m_engine->addFile(data, name, mime, [self, noteId, name](AddedFile f) {
        if (!self) return;
        self->m_uploading--;
        self->refresh();
        if (!f.ok) {
            Toaster::show((i18n::isZh() ? QStringLiteral("无法添加 %1：%2") : QStringLiteral("Could not add %1: %2")).arg(name, f.error), true);
            return;
        }
        QString label = f.name.isEmpty() ? name : f.name;
        label.remove(u'[').remove(u']');
        const QString md = (f.kind == QLatin1String("file") ? QString() : QStringLiteral("!")) + QStringLiteral("[%1](ivy://res/%2)").arg(label, f.id);
        if (self->m_id == noteId) {
            self->m_editor->insertBlockText(md);
        } else if (const Note *n = self->m_engine->note(noteId)) {
            NotePatch p;
            p.body = n->body + (n->body.endsWith(u'\n') || n->body.isEmpty() ? QString() : QStringLiteral("\n")) + md + u'\n';
            self->m_engine->updateNote(noteId, p);
        }
        if (f.local) Toaster::show(T("离线中，文件将在联网后上传"));
    });
}

void NotePane::insertFiles(const QStringList &paths) {
    QMimeDatabase db;
    for (const QString &path : paths) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QByteArray data = f.readAll();
        attach(data, QFileInfo(path).fileName(), db.mimeTypeForFileNameAndData(path, data).name());
    }
}

void NotePane::pickFiles() {
    if (m_id.isEmpty()) return;
    const QStringList files = QFileDialog::getOpenFileNames(this, T("插入图片或文件"));
    if (!files.isEmpty()) insertFiles(files);
}

void NotePane::addImage(const QImage &img) {
    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    attach(png, QStringLiteral("image-%1.png").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))), QStringLiteral("image/png"));
}

void NotePane::pickTags() {
    if (!m_engine || m_id.isEmpty()) return;
    dialogs::tagPicker(m_engine, m_id, m_tagsRow->mapToGlobal(QPoint(0, m_tagsRow->height() + 4)), this);
}

void NotePane::showMoreMenu() {
    if (!m_engine || m_id.isEmpty()) return;
    const Note *n = m_engine->note(m_id);
    if (!n) return;
    QMenu menu(this);
    // Editor mode as a segmented choice.
    auto *modes = new QWidget(&menu);
    auto *row = new QHBoxLayout(modes);
    row->setContentsMargins(10, 6, 10, 6);
    row->setSpacing(2);
    const QList<QPair<EditorMode, const char *>> list{{EditorMode::Live, "即时渲染"}, {EditorMode::Source, "源码"}, {EditorMode::Split, "双栏"}};
    for (const auto &[mode, label] : list) {
        auto *b = new QPushButton(T(label), modes);
        b->setCheckable(true);
        b->setChecked(m_mode == mode);
        b->setFlat(m_mode != mode);
        connect(b, &QPushButton::clicked, &menu, [this, &menu, m = mode] {
            setEditorMode(m);
            menu.close();
        });
        row->addWidget(b);
    }
    auto *wa = new QWidgetAction(&menu);
    wa->setDefaultWidget(modes);
    menu.addAction(wa);
    menu.addSeparator();
    const QString id = m_id;
    if (!n->trashed()) {
        menu.addAction(icons::themed("pin"), n->pinned ? T("取消置顶") : T("设为置顶"), this, [this, id, pinned = n->pinned] {
            NotePatch p;
            p.pinned = !pinned;
            m_engine->updateNote(id, p);
        });
        menu.addAction(icons::themed("move"), T("移动到…"), this, [this, id] { emit moveRequested(id); });
        menu.addAction(icons::themed("tag"), T("标签…"), this, &NotePane::pickTags);
        menu.addAction(icons::themed("paperclip"), T("插入图片或文件…"), this, &NotePane::pickFiles);
        menu.addAction(icons::themed("outline"), T("大纲"), this, &NotePane::toggleOutline);
        menu.addAction(icons::themed("copy"), T("复制笔记"), this, [this, id] { emit duplicateRequested(id); });
        if (m_session && m_session->isRemote()) menu.addAction(icons::themed("history"), T("历史版本"), this, [this, id] { emit historyRequested(id); });
        menu.addSeparator();
        menu.addAction(icons::themed("trash"), T("移到回收站"), this, [this, id] {
            flush();
            NotePatch p;
            p.trashed = true;
            m_engine->updateNote(id, p);
            QPointer<SyncEngine> e(m_engine);
            Toaster::show(T("已移到回收站"), false, T("撤销"), [e, id] {
                NotePatch u;
                u.trashed = false;
                if (e) e->updateNote(id, u);
            });
        });
    }
    menu.exec(m_moreButton->mapToGlobal(QPoint(m_moreButton->width() - menu.sizeHint().width(), m_moreButton->height())));
}

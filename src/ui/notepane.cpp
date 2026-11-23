#include "ui/notepane.h"

#include "core/diff.h"
#include "core/format.h"
#include "core/i18n.h"
#include "core/markdown.h"
#include "core/models.h"
#include "core/notelock.h"
#include "sync/engine.h"
#include "ui/dialogs.h"
#include "ui/editor.h"
#include "ui/highlighter.h"
#include "ui/icons.h"
#include "ui/markdownview.h"
#include "ui/noteinfo.h"
#include "ui/render.h"
#include "ui/theme.h"
#include "ui/toast.h"
#include "ui/widgets.h"
#include "vault/session.h"

#include <QAbstractTextDocumentLayout>
#include <QActionGroup>
#include <QBuffer>
#include <QComboBox>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QShortcut>
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

// The line under the title (time and word count): clicking it opens the note info.
class MetaLabel : public QLabel {
public:
    explicit MetaLabel(QWidget *parent) : QLabel(parent) {
        setCursor(Qt::PointingHandCursor);
        setFont(Theme::uiFont(12.5));
        paint(false);
        connect(Theme::instance(), &Theme::changed, this, [this] { paint(m_hover); });
    }
    std::function<void()> clicked;

protected:
    void enterEvent(QEnterEvent *) override { paint(true); }
    void leaveEvent(QEvent *) override { paint(false); }
    void mouseReleaseEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton && rect().contains(e->position().toPoint()) && clicked) clicked();
    }

private:
    void paint(bool hover) {
        m_hover = hover;
        QPalette pal = palette();
        pal.setColor(QPalette::WindowText, hover ? Theme::t().ink2 : Theme::t().muted);
        setPalette(pal);
    }
    bool m_hover = false;
};

// The note's actions: they float in the top right corner over the page
// instead of taking a row of their own, so the title can start at the top.
class FloatingBar : public QWidget {
public:
    explicit FloatingBar(QWidget *parent) : QWidget(parent) {}
    std::function<void()> relayout;

protected:
    bool event(QEvent *e) override {
        const bool handled = QWidget::event(e);
        if (e->type() == QEvent::LayoutRequest && relayout) relayout();
        return handled;
    }
};

// The outline starts below the floating actions.
class OutlineList : public QListWidget {
public:
    explicit OutlineList(QWidget *parent) : QListWidget(parent) { setViewportMargins(0, 44, 0, 0); }
};

} // namespace

NotePane::NotePane(QWidget *parent) : QWidget(parent) {
    // The note on the left; the outline, when open, runs the full height beside it.
    auto *outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto *main = new QWidget(this);
    outer->addWidget(main, 1);
    auto *col = new QVBoxLayout(main);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);

    auto *floating = new FloatingBar(this);
    floating->relayout = [this] { placeTopBar(); };
    m_topBar = floating;
    auto *top = new QHBoxLayout(m_topBar);
    top->setContentsMargins(0, 0, 0, 0);
    top->setSpacing(2);
    m_insertButton = new IconButton(QStringLiteral("plus"), T("插入") + QStringLiteral(" (Ctrl+/)"), m_topBar);
    m_insertButton->setObjectName(QStringLiteral("insertButton"));
    connect(m_insertButton, &QToolButton::clicked, this, [this] { m_editor->showInsertMenu(); });
    top->addWidget(m_insertButton);

    m_dueButton = new IconButton(QStringLiteral("calendar"), T("设置日期与重复") + QStringLiteral(" (Ctrl+Shift+D)"), m_topBar);
    m_dueButton->setObjectName(QStringLiteral("dueButton"));
    connect(m_dueButton, &QToolButton::clicked, this, [this] { m_editor->openDuePicker(); });
    top->addWidget(m_dueButton);

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
    m_topBar->adjustSize();
    m_topBar->raise();

    // Trash banner.
    m_banner = new QWidget(this);
    auto *bannerRow = new QHBoxLayout(m_banner);
    bannerRow->setContentsMargins(24, 8, 260, 8); // clear of the floating actions
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

    // Sync conflict banner.
    m_conflictBanner = new QWidget(this);
    m_conflictBanner->setObjectName(QStringLiteral("conflictBanner"));
    auto *conflictRow = new QHBoxLayout(m_conflictBanner);
    conflictRow->setContentsMargins(24, 8, 260, 8);
    m_conflictLabel = new QLabel(T("这是一份同步冲突副本，建议与原笔记对比后合并。"), m_conflictBanner);
    conflictRow->addWidget(m_conflictLabel, 1);
    auto *diffAction = new PillButton(T("对比差异 (Diff)"), PillButton::Secondary, m_conflictBanner);
    conflictRow->addWidget(diffAction);
    connect(diffAction, &QToolButton::clicked, this, [this] {
        if (!m_id.isEmpty()) emit diffRequested(m_id, m_conflictTargetId);
    });
    m_conflictBanner->setAutoFillBackground(true);
    m_conflictBanner->hide();
    col->addWidget(m_conflictBanner);

    // Title and meta, on the same column as the text.
    m_headHost = new QWidget(this);
    auto *head = m_head = new QWidget(m_headHost);
    auto *headCol = new QVBoxLayout(head);
    headCol->setContentsMargins(0, 22, 0, 6);
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
    auto *meta = new MetaLabel(metaRow);
    m_meta = meta;
    meta->clicked = [this] { showInfo(); };
    metaLayout->addWidget(m_meta);
    m_tagsRow = new FlowRow(metaRow);
    metaLayout->addWidget(m_tagsRow, 1);
    m_pendingDot = new QLabel(metaRow);
    m_pendingDot->setFixedSize(14, 14);
    m_pendingDot->setToolTip(T("有待同步的修改"));
    metaLayout->addWidget(m_pendingDot);
    m_savedLabel = mutedLabel(T("已保存到本机"), metaRow, 12);
    m_savedLabel->hide();
    metaLayout->addWidget(m_savedLabel);
    headCol->addWidget(metaRow);
    auto *headRow = new QHBoxLayout(m_headHost);
    headRow->setContentsMargins(24, 0, 24, 0);
    headRow->addWidget(head);
    col->addWidget(m_headHost);
    m_findBar = new QWidget(this);
    m_findBar->setObjectName(QStringLiteral("noteFindBar"));
    auto *findCol = new QVBoxLayout(m_findBar);
    findCol->setContentsMargins(24, 4, 24, 4);
    auto *findRow = new QHBoxLayout;
    findCol->addLayout(findRow);
    m_findText = new QLineEdit(m_findBar);
    m_findText->setObjectName(QStringLiteral("noteFindText"));
    m_findText->setPlaceholderText(T("在笔记中查找"));
    m_findCount = new QLabel(m_findBar);
    m_findCase = new QToolButton(m_findBar);
    m_findCase->setText(QStringLiteral("Aa"));
    m_findCase->setToolTip(T("区分大小写"));
    m_findCase->setCheckable(true);
    auto *previous = new QToolButton(m_findBar);
    previous->setText(QStringLiteral("↑")); previous->setToolTip(T("上一个"));
    auto *next = new QToolButton(m_findBar);
    next->setText(QStringLiteral("↓")); next->setToolTip(T("下一个"));
    auto *closeFind = new QToolButton(m_findBar);
    closeFind->setText(QStringLiteral("×")); closeFind->setToolTip(T("关闭"));
    auto *toggleReplace = new QToolButton(m_findBar);
    toggleReplace->setObjectName(QStringLiteral("noteReplaceToggle"));
    toggleReplace->setText(QStringLiteral("↳"));
    toggleReplace->setToolTip(T("查找与替换"));
    toggleReplace->setCheckable(true);
    findRow->addWidget(toggleReplace);
    findRow->addWidget(m_findText, 1);
    findRow->addWidget(m_findCount);
    findRow->addWidget(m_findCase);
    findRow->addWidget(previous);
    findRow->addWidget(next);
    findRow->addWidget(closeFind);
    m_replaceRow = new QWidget(m_findBar);
    m_replaceRow->setObjectName(QStringLiteral("noteReplaceRow"));
    auto *replaceRow = new QHBoxLayout(m_replaceRow);
    replaceRow->setContentsMargins(0, 0, 0, 0);
    m_replaceText = new QLineEdit(m_replaceRow);
    m_replaceText->setObjectName(QStringLiteral("noteReplaceText"));
    m_replaceText->setPlaceholderText(T("替换为"));
    m_replaceOne = new QToolButton(m_replaceRow);
    m_replaceOne->setText(T("替换"));
    m_replaceOne->setObjectName(QStringLiteral("noteReplaceOne"));
    m_replaceAll = new QToolButton(m_replaceRow);
    m_replaceAll->setText(T("全部替换"));
    m_replaceAll->setObjectName(QStringLiteral("noteReplaceAll"));
    replaceRow->addWidget(m_replaceText, 1);
    replaceRow->addWidget(m_replaceOne);
    replaceRow->addWidget(m_replaceAll);
    findCol->addWidget(m_replaceRow);
    m_replaceRow->hide();
    connect(toggleReplace, &QToolButton::toggled, m_replaceRow, &QWidget::setVisible);
    connect(m_replaceOne, &QToolButton::clicked, this, [this] { replaceMatches(false); });
    connect(m_replaceAll, &QToolButton::clicked, this, [this] { replaceMatches(true); });
    connect(m_replaceText, &QLineEdit::returnPressed, this, [this] { replaceMatches(false); });
    m_findBar->hide();
    col->addWidget(m_findBar);
    connect(m_findText, &QLineEdit::textChanged, this, [this] { refreshFind(); if (!m_findText->text().isEmpty()) findNext(); });
    connect(m_findText, &QLineEdit::returnPressed, this, [this] { findNext(); });
    connect(m_findCase, &QToolButton::toggled, this, [this] { refreshFind(); });
    connect(previous, &QToolButton::clicked, this, [this] { findNext(true); });
    connect(next, &QToolButton::clicked, this, [this] { findNext(); });
    auto closeSearch = [this] {
        m_findBar->hide();
        m_editor->setExtraSelections({});
        m_preview->setExtraSelections({});
        focusEditor();
    };
    connect(closeFind, &QToolButton::clicked, this, closeSearch);
    auto *escapeFind = new QShortcut(QKeySequence(Qt::Key_Escape), m_findBar);
    escapeFind->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escapeFind, &QShortcut::activated, this, closeSearch);
    auto *findShortcut = new QShortcut(QKeySequence::Find, this);
    findShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(findShortcut, &QShortcut::activated, this, &NotePane::showFind);
    auto *replaceShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+H")), this);
    replaceShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(replaceShortcut, &QShortcut::activated, this, &NotePane::showReplace);
    for (bool back : {false, true}) {
        auto *shortcut = new QShortcut(QKeySequence(back ? QStringLiteral("Shift+F3") : QStringLiteral("F3")), this);
        shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(shortcut, &QShortcut::activated, this, [this, back] { if (!m_findBar->isVisible()) showFind(); else findNext(back); });
    }


    // Editor, preview and outline.
    auto *body = m_body = new QWidget(this);
    auto *bodyRow = new QHBoxLayout(body);
    bodyRow->setContentsMargins(0, 0, 0, 0);
    bodyRow->setSpacing(0);
    m_split = new Divider(Qt::Horizontal, body);
    m_split->setChildrenCollapsible(false);
    m_editor = new MarkdownEditor(m_split);
    passAppShortcuts(m_editor);
    m_preview = new MarkdownView(m_split);
    passAppShortcuts(m_preview);
    m_split->addWidget(m_editor);
    m_split->addWidget(m_preview);
    m_preview->hide();
    bodyRow->addWidget(m_split, 1);
    m_outline = new OutlineList(this);
    m_outline->setFixedWidth(224);
    m_outline->setFrameShape(QFrame::NoFrame);
    m_outline->setFont(Theme::uiFont(13));
    m_outline->hide();
    outer->addWidget(m_outline);
    col->addWidget(body, 1);

    // Lock panel: stands in for the editor while a protected note is locked.
    m_lockPanel = new QWidget(this);
    auto *lockOuter = new QVBoxLayout(m_lockPanel);
    lockOuter->setContentsMargins(24, 24, 24, 80);
    lockOuter->addStretch(1);
    auto *lockCol = new QVBoxLayout;
    lockCol->setSpacing(10);
    lockCol->setAlignment(Qt::AlignHCenter);
    m_lockIcon = new QLabel(m_lockPanel);
    m_lockIcon->setAlignment(Qt::AlignCenter);
    auto *lockTitle = new QLabel(T("这篇笔记已加密"), m_lockPanel);
    lockTitle->setAlignment(Qt::AlignCenter);
    lockTitle->setFont(Theme::serif(20));
    m_lockHint = mutedLabel(QString(), m_lockPanel, 12.5);
    m_lockHint->setAlignment(Qt::AlignCenter);
    m_lockHint->setWordWrap(true);
    m_lockPassword = new QLineEdit(m_lockPanel);
    m_lockPassword->setObjectName(QStringLiteral("noteLockPassword"));
    m_lockPassword->setEchoMode(QLineEdit::Password);
    m_lockPassword->setPlaceholderText(T("输入密码"));
    m_lockPassword->setFixedWidth(280);
    m_lockError = new QLabel(m_lockPanel);
    m_lockError->setObjectName(QStringLiteral("noteLockError"));
    m_lockError->setAlignment(Qt::AlignCenter);
    m_lockError->setWordWrap(true);
    m_lockButton = new PillButton(T("解锁"), PillButton::Primary, m_lockPanel);
    m_lockButton->setObjectName(QStringLiteral("noteLockButton"));
    for (QWidget *w : {static_cast<QWidget *>(m_lockIcon), static_cast<QWidget *>(lockTitle), static_cast<QWidget *>(m_lockHint),
                       static_cast<QWidget *>(m_lockPassword), static_cast<QWidget *>(m_lockError), static_cast<QWidget *>(m_lockButton)})
        lockCol->addWidget(w, 0, Qt::AlignHCenter);
    lockOuter->addLayout(lockCol);
    lockOuter->addStretch(2);
    m_lockPanel->hide();
    col->addWidget(m_lockPanel, 1);
    connect(m_lockButton, &QToolButton::clicked, this, &NotePane::tryUnlock);
    connect(m_lockPassword, &QLineEdit::returnPressed, this, &NotePane::tryUnlock);
    passAppShortcuts(m_lockPassword);
    // A key idle for ten minutes is forgotten: the open note locks again.
    m_lockTimer.setInterval(15 * 1000);
    connect(&m_lockTimer, &QTimer::timeout, this, [this] {
        if (m_sealed && !notelock::hasLiveKey(m_lockKey)) {
            relock();
            Toaster::show(T("这篇笔记已自动锁定"));
        }
    });
    m_lockTimer.start();

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
        const QString slug = it->data(Qt::UserRole + 1).toString();
        if (m_reading) {
            m_preview->scrollToAnchor(slug);
            return;
        }
        if (m_mode == EditorMode::Split) m_preview->scrollToAnchor(slug);
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
    connect(m_preview, &MarkdownView::columnChanged, this, &NotePane::alignHead);
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
        if (m_sealed) notelock::touch(m_lockKey);
        refreshFind();
        m_bodyDirty = true;
        m_bodyTimer.start();
        if (m_mode == EditorMode::Split || m_reading) m_previewTimer.start();
    });
    // Reading and moving around count as use too.
    connect(m_editor, &QTextEdit::cursorPositionChanged, this, [this] { if (m_sealed) notelock::touch(m_lockKey); });
    connect(m_editor->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { if (m_sealed) notelock::touch(m_lockKey); });
    connect(m_preview->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { if (m_sealed) notelock::touch(m_lockKey); });
    connect(m_title, &QLineEdit::textEdited, this, [this] {
        m_titleDirty = true;
        m_titleTimer.start();
    });
    connect(m_title, &QLineEdit::returnPressed, this, [this] {
        // Enter in the title goes to the start of the text.
        flush();
        if (m_reading) return;
        QTextCursor c = m_editor->textCursor();
        c.movePosition(QTextCursor::Start);
        m_editor->setTextCursor(c);
        m_editor->setFocus();
    });
    connect(m_editor, &MarkdownEditor::linkActivated, this, &NotePane::openLink);
    connect(m_editor, &MarkdownEditor::attachmentRequested, this, &NotePane::pickFiles);
    connect(m_editor, &MarkdownEditor::filesDropped, this, &NotePane::insertFiles);
    connect(m_editor, &MarkdownEditor::imagePasted, this, &NotePane::addImage);
    connect(m_preview, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) {
        const QString s = url.toString();
        if (s.startsWith(QLatin1String("ivy-task:"))) {
            if (!m_reading && m_engine && m_engine->note(m_id) && !m_editor->isReadOnly()) {
                flush();
                const QString body = md::toggleTask(m_editor->markdown(), s.mid(9).toInt());
                if (body != m_editor->markdown()) {
                    m_editor->applyRemote(body);
                    m_bodyDirty = true;
                    flush();
                    refreshPreview();
                }
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
        m_conflictBanner->setPalette(bp);
        QPalette tp = m_title->palette();
        tp.setColor(QPalette::Base, k.surface);
        tp.setColor(QPalette::Text, k.ink);
        m_title->setPalette(tp);
        for (QWidget *w : {static_cast<QWidget *>(m_outline), static_cast<QWidget *>(m_backlinks)}) {
            QPalette lp = w->palette();
            lp.setColor(QPalette::Base, k.surface);
            lp.setColor(QPalette::Text, k.ink2);
            w->setPalette(lp);
        }
        m_backlinksButton->setIcon(icons::icon(QStringLiteral("backlink"), k.muted));
        m_lockIcon->setPixmap(icons::icon(QStringLiteral("lock"), k.muted).pixmap(QSize(40, 40)));
        QPalette ep = m_lockError->palette();
        ep.setColor(QPalette::WindowText, k.danger);
        m_lockError->setPalette(ep);
        refresh();
        refreshPreview();
    };
    connect(Theme::instance(), &Theme::changed, this, restyle);
    restyle();
    setNote(QString());
}

void NotePane::setSession(Session *session) {
    flush();
    if (m_session != session) notelock::forgetAll(); // keys never outlive their library
    m_sealed = m_locked = false;
    m_lockKey = {};
    if (m_engine) m_engine->disconnect(this);
    if (m_session) m_session->disconnect(this);
    m_session = session;
    m_engine = session ? session->engine() : nullptr;
    m_id.clear();
    m_editor->clearImageCache();
    m_preview->clearImageCache();
    if (!session) return setNote(QString());
    m_shareButton->setVisible(session->isRemote());
    connect(m_engine, &SyncEngine::aboutToApplyRemote, this, &NotePane::flush, Qt::DirectConnection);
    connect(m_engine, &SyncEngine::remoteBody, this, [this](const QString &id, const QString &body) {
        if (id == m_id && !m_bodyDirty) onRemoteBody(body);
    });
    connect(m_engine, &SyncEngine::resourceNormalized, this, [this](const QString &localId, const QString &remoteId) {
        if (m_id.isEmpty() || m_locked) return;
        if (m_editor->replaceResourceId(localId, remoteId)) {
            if (m_sealed) {
                // Re-seal unlocked protected note so the normalized server ID is saved & synced.
                const QString sealed = notelock::seal(m_editor->markdown(), m_lockKey);
                if (!sealed.isEmpty()) {
                    notelock::touch(m_lockKey);
                    NotePatch p;
                    p.body = sealed;
                    m_engine->updateNote(m_id, p);
                }
            }
            if (m_mode == EditorMode::Split || m_reading) m_previewTimer.start();
        }
    });
    connect(m_engine, &SyncEngine::dataChanged, this, [this] {
        refresh();
        m_indexTimer.start();
    });
    connect(m_engine, &SyncEngine::stateChanged, this, &NotePane::refresh);
    connect(session, &Session::attachmentReady, this, [this](const QString &id) {
        m_editor->refreshAttachment(id);
        if (m_mode == EditorMode::Split || m_reading) m_preview->refreshAttachment(id);
    });
    QPointer<Session> s(session);
    m_editor->image = [s](const QString &id) -> QImage {
        if (!s) return {};
        const Attachment a = s->engine()->attachments().value(id);
        return s->image(id, s->isRemote() && a.hasThumb);
    };
    m_editor->cachedImage = [s](const QString &id) -> QImage {
        if (!s) return {};
        const Attachment a = s->engine()->attachments().value(id);
        return s->cachedImage(id, s->isRemote() && a.hasThumb);
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
    m_preview->image = [s](const QString &id) { return s ? s->image(id, false) : QImage(); };
    m_preview->cachedImage = [s](const QString &id) { return s ? s->cachedImage(id, false) : QImage(); };
    m_preview->imageMeta = [s](const QString &id) -> QSize {
        if (!s) return {};
        const Attachment a = s->engine()->attachments().value(id);
        if (a.width <= 0 || a.height <= 0) return {};
        return QSize(a.width, a.height);
    };
    refreshIndex();
    setNote(QString());
}

void NotePane::setNote(const QString &id) {
    flush();
    m_findBar->hide();
    m_editor->setExtraSelections({});
    m_preview->setExtraSelections({});
    m_id = id;
    m_bodyDirty = m_titleDirty = false;
    const Note *n = m_engine && !id.isEmpty() ? m_engine->note(id) : nullptr;
    if (!n) m_id.clear();
    const bool has = n != nullptr;
    for (QWidget *w : findChildren<QWidget *>(Qt::FindDirectChildrenOnly)) w->setEnabled(has);
    m_title->setText(n ? n->title : QString());
    loadBody(n ? n->body : QString());
    m_editor->setReadOnly(!n || n->trashed() || m_locked);
    m_backlinks->hide();
    applyViewState();
    refresh();
    refreshBacklinks();
    refreshOutline();
    if (m_mode == EditorMode::Split || m_reading) refreshPreview();
    if (has) {
        // Note to note: a quick cross-fade, not a cut.
        rise(m_headHost, 180);
        rise(m_locked ? m_lockPanel : m_body, 180);
    }
}

void NotePane::flush() {
    m_editor->commitPendingCell();
    if (!m_engine || m_id.isEmpty()) return;
    m_bodyTimer.stop();
    m_titleTimer.stop();
    if (!m_bodyDirty && !m_titleDirty) return;
    const Note *n = m_engine->note(m_id);
    if (!n || n->trashed()) {
        m_bodyDirty = m_titleDirty = false;
        return;
    }
    // A locked note's editor is empty and not the note's text: never save it.
    if (m_locked) m_bodyDirty = false;
    NotePatch p;
    if (m_bodyDirty) {
        if (m_sealed) {
            // The plaintext stays in this editor; only the sealed body is saved.
            const QString sealed = notelock::seal(m_editor->markdown(), m_lockKey);
            if (sealed.isEmpty()) {
                Toaster::show(T("无法加密这篇笔记，修改未保存。"), true);
                m_bodyDirty = false;
            } else {
                p.body = sealed;
                notelock::touch(m_lockKey);
            }
        } else {
            p.body = m_editor->markdown();
        }
    }
    if (m_titleDirty) p.title = m_title->text().simplified().left(300);
    m_bodyDirty = m_titleDirty = false;
    if (!p.body && !p.title) return;
    m_engine->updateNote(m_id, p);
    if (!m_savedLabel->isVisible()) {
        m_savedLabel->show();
        rise(m_savedLabel, 200);
    }
    m_savedTimer.start();
    if (p.body) refreshOutline();
}

// --- Password-protected notes (guide 8.4) ---

void NotePane::loadBody(const QString &body) {
    m_sealed = m_locked = false;
    m_lockKey = {};
    QString text = body;
    if (notelock::isEncrypted(body)) {
        QString plain;
        notelock::Key key;
        if (notelock::openRemembered(body, &plain, &key)) {
            m_sealed = true;
            m_lockKey = key;
            text = plain;
        } else {
            m_locked = true;
            text.clear();
        }
    }
    m_editor->setMarkdown(text);
    showLockPanel(m_locked, m_locked ? notelock::passwordHint(body) : QString());
}

void NotePane::showLockPanel(bool locked, const QString &hint) {
    m_lockPanel->setVisible(locked);
    m_body->setVisible(!locked);
    applyViewState();
    if (!locked) return;
    m_findBar->hide();
    m_lockHint->setText(hint.isEmpty() ? QString() : T("密码提示：") + hint);
    m_lockHint->setVisible(!hint.isEmpty());
    m_lockPassword->clear();
    m_lockError->clear();
    m_lockPassword->setEnabled(!m_unlocking);
    m_lockButton->setEnabled(!m_unlocking);
    if (isVisible() && !m_unlocking) m_lockPassword->setFocus();
}

void NotePane::reloadFromEngine() {
    const Note *n = m_engine && !m_id.isEmpty() ? m_engine->note(m_id) : nullptr;
    if (!n) return;
    loadBody(n->body);
    m_editor->setReadOnly(n->trashed() || m_locked);
    applyViewState();
    refresh();
    refreshOutline();
    if (m_mode == EditorMode::Split || m_reading) refreshPreview();
}

void NotePane::syncBodyFromEngine() {
    if (!m_engine || m_id.isEmpty() || m_bodyDirty) return;
    if (const Note *n = m_engine->note(m_id)) {
        onRemoteBody(n->body);
        refreshOutline();
    }
}

void NotePane::onRemoteBody(const QString &body) {
    // A body from another device or the merge of ours. Plain notes just take
    // it; protected ones are opened with the key we hold, or the lock panel
    // comes back (the password was changed elsewhere, or the note was
    // protected or unprotected there).
    const bool encrypted = notelock::isEncrypted(body);
    if (!encrypted && !m_sealed && !m_locked) {
        if (m_editor->markdown() != body) {
            m_editor->applyRemote(body);
            if (m_mode == EditorMode::Split || m_reading) refreshPreview();
        }
        return;
    }
    if (encrypted && m_sealed) {
        QString plain;
        if (notelock::open(body, m_lockKey, &plain) == notelock::Status::Ok) {
            if (m_editor->markdown() != plain) {
                m_editor->applyRemote(plain);
                if (m_mode == EditorMode::Split || m_reading) refreshPreview();
            }
            return;
        }
    }
    reloadFromEngine();
}

void NotePane::tryUnlock() {
    if (!m_locked || m_unlocking || !m_engine) return;
    const QString password = m_lockPassword->text();
    const Note *n = m_engine->note(m_id);
    if (password.isEmpty() || !n) return;
    struct Result {
        notelock::Status status = notelock::Status::Broken;
        notelock::Key key;
    };
    m_unlocking = true;
    m_lockPassword->setEnabled(false);
    m_lockButton->setEnabled(false);
    m_lockError->clear();
    const QString id = m_id;
    const QString body = n->body;
    auto *watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, id] {
        const Result r = watcher->result();
        watcher->deleteLater();
        m_unlocking = false;
        m_lockPassword->setEnabled(true);
        m_lockButton->setEnabled(true);
        if (r.status == notelock::Status::Ok) {
            // The key is remembered now; show the note it opens. When the user
            // already moved on, the remembered key makes the next visit instant.
            if (m_id == id) {
                m_lockPassword->clear();
                reloadFromEngine();
                focusEditor();
            }
            return;
        }
        if (m_id != id) return;
        m_lockError->setText(r.status == notelock::Status::WrongPassword ? T("密码不正确。") : T("这篇笔记的加密数据已损坏，无法打开。"));
        m_lockPassword->selectAll();
        m_lockPassword->setFocus();
    });
    watcher->setFuture(QtConcurrent::run([body, password] {
        Result r;
        QString plain;
        r.status = notelock::unlock(body, password, &plain, &r.key);
        return r;
    }));
}

void NotePane::relock() {
    if (!m_sealed) return;
    flush();
    notelock::forget(m_lockKey);
    reloadFromEngine();
}

void NotePane::protectNote(bool change) {
    if (!m_engine || m_id.isEmpty() || m_locked || m_protecting) return;
    const Note *n = m_engine->note(m_id);
    if (!n || n->trashed()) return;
    QString password, hint;
    if (!dialogs::askNotePassword(this, change, &password, &hint)) return;
    flush();
    m_protecting = true;
    Toaster::show(T("正在加密…"));
    const QString id = m_id;
    auto *watcher = new QFutureWatcher<notelock::Key>(this);
    connect(watcher, &QFutureWatcher<notelock::Key>::finished, this, [this, watcher, id] {
        const notelock::Key key = watcher->result();
        watcher->deleteLater();
        m_protecting = false;
        if (!key.valid()) {
            Toaster::show(T("无法加密这篇笔记。"), true);
            return;
        }
        const Note *cur = m_engine ? m_engine->note(id) : nullptr;
        if (m_id != id || !cur || cur->trashed() || m_locked) {
            Toaster::show(T("笔记已切换或不可编辑，没有加密。"), true);
            return;
        }
        // Typing may have gone on while the key was derived: seal what the
        // editor holds now.
        m_editor->commitPendingCell();
        m_bodyTimer.stop();
        m_bodyDirty = false;
        const QString sealed = notelock::seal(m_editor->markdown(), key);
        if (sealed.isEmpty()) {
            Toaster::show(T("无法加密这篇笔记。"), true);
            return;
        }
        if (m_sealed) notelock::forget(m_lockKey);
        m_sealed = true;
        m_lockKey = key;
        notelock::remember(key);
        NotePatch p;
        p.body = sealed;
        m_engine->updateNote(id, p);
        refresh();
        Toaster::show(T("已加密。忘记密码无法恢复。"));
    });
    watcher->setFuture(QtConcurrent::run([password, hint] { return notelock::newKey(password, hint); }));
}

void NotePane::removeProtection() {
    if (!m_engine || m_id.isEmpty() || !m_sealed) return;
    if (!dialogs::confirm(this, T("移除密码"), T("移除密码后，这篇笔记会以明文保存和同步，服务器可以看到内容。继续吗？"), T("移除密码"))) return;
    m_editor->commitPendingCell();
    m_bodyTimer.stop();
    m_bodyDirty = false;
    const QString plain = m_editor->markdown();
    if (notelock::isEncrypted(plain)) {
        // Saving it would turn the note back into an unreadable envelope.
        Toaster::show(T("正文本身是一段加密数据，无法移除密码。"), true);
        return;
    }
    notelock::forget(m_lockKey);
    m_sealed = false;
    m_lockKey = {};
    NotePatch p;
    p.body = plain;
    m_engine->updateNote(m_id, p);
    refresh();
    Toaster::show(T("已移除密码"));
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
    m_conflictTargetId.clear();
    if (!n) {
        m_conflictBanner->hide();
        m_meta->clear();
        m_tagsRow->clear();
        return;
    }
    const bool showConflict = !n->trashed() && !m_locked;
    if (showConflict && m_engine) {
        const auto counterparts = diff::findConflictCounterparts(*n, m_engine->notes());
        if (!counterparts.isEmpty()) m_conflictTargetId = counterparts.first()->id;
        const bool isCopy = diff::isConflictNote(n->title);
        const bool hasConflict = isCopy || !counterparts.isEmpty();
        m_conflictBanner->setVisible(hasConflict);
        if (hasConflict) {
            m_conflictLabel->setText(isCopy ? T("这是一份同步冲突副本，建议与原笔记对比后合并。")
                                            : T("发现该笔记存在同步冲突副本，建议对比差异。"));
        }
    } else {
        m_conflictBanner->hide();
    }
    // Keep the title in step with other devices unless it is being typed.
    if (!m_titleDirty && !m_title->hasFocus() && m_title->text() != n->title) m_title->setText(n->title);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool protectedNote = notelock::isEncrypted(n->body);
    m_shareButton->setVisible(m_session && m_session->isRemote() && !protectedNote); // protected notes cannot be shared
    if (protectedNote) {
        // The count of a sealed body would be the ciphertext's.
        m_meta->setText(QStringLiteral("%1   %2").arg(fmt::fullTime(n->updatedAt, now), T("已加密")));
    } else {
        const int words = md::wordCount(n->body);
        m_meta->setText(QStringLiteral("%1   %2 %3").arg(fmt::fullTime(n->updatedAt, now), fmt::number(words), T("字")));
    }
    m_meta->setToolTip(T("笔记信息") + QStringLiteral("\n") + T("创建于") + u' ' + fmt::fullTime(n->createdAt, now));
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
    int queuedUploads = 0;
    if (m_engine && !m_engine->isLocalOnly() && m_engine->state().status != SyncState::Offline) {
        for (const auto &u : m_engine->uploads())
            if (u.failed.isEmpty()) queuedUploads++;
    }
    if (m_uploading > 0 || queuedUploads > 0) {
        m_savedLabel->setText(T("正在上传…"));
        m_savedLabel->show();
    } else {
        m_savedLabel->setText(T("已保存到本机"));
    }
}

QString NotePane::resolveTitle(const QString &title) const { return m_titleIndex.value(md::normalizeTitle(title)); }

void NotePane::refreshIndex() {
    refreshTitleIndex();
    refreshBacklinks();
}

void NotePane::refreshTitleIndex() {
    if (!m_engine) return;
    // Titles: newest wins on duplicates; trash excluded.
    QHash<QString, QPair<QString, qint64>> best;
    QStringList titles;
    for (const auto &n : m_engine->notes()) {
        if (n.trashed() || n.title.trimmed().isEmpty()) continue;
        const QString key = md::normalizeTitle(n.title);
        auto it = best.find(key);
        if (it == best.end() || it->second < n.updatedAt) best[key] = {n.id, n.updatedAt};
        titles << n.title;
    }
    m_titleIndex.clear();
    for (auto it = best.begin(); it != best.end(); ++it) m_titleIndex.insert(it.key(), it->first);
    titles.removeDuplicates();
    titles.sort(Qt::CaseInsensitive);
    m_editor->setWikiTitles(titles);
}

void NotePane::refreshBacklinks() {
    if (!m_engine) return;
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
    static const QRegularExpression quickWiki(QStringLiteral(R"(\[\[([^\[\]\n]+?)\]\])"));
    QList<const Note *> linking;
    for (const auto &n : m_engine->notes()) {
        if (n.id == cur->id || n.trashed()) continue;
        bool hit = false;
        if (isTarget && n.body.contains(QLatin1String("[["))) {
            bool maybe = false;
            for (auto it = quickWiki.globalMatch(n.body); it.hasNext();) {
                if (md::normalizeTitle(md::parseWiki(it.next().captured(1)).target) == key) {
                    maybe = true;
                    break;
                }
            }
            if (maybe) {
                for (const QString &t : md::wikiTargets(n.body))
                    if (t == key) {
                        hit = true;
                        break;
                    }
            }
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

QTextEdit *NotePane::activeTextView() const {
    return m_reading ? static_cast<QTextEdit *>(m_preview) : static_cast<QTextEdit *>(m_editor);
}

void NotePane::applyViewState() {
    const Note *n = m_engine && !m_id.isEmpty() ? m_engine->note(m_id) : nullptr;
    const bool has = n != nullptr;
    const bool trashed = n && n->trashed();
    const bool editable = has && !trashed && !m_locked && !m_reading;

    m_title->setReadOnly(!editable);
    m_insertButton->setVisible(editable);
    m_dueButton->setVisible(editable);
    m_replaceOne->setEnabled(editable);
    m_replaceAll->setEnabled(editable);
    if (!editable && m_replaceRow->isVisible()) {
        if (auto *t = findChild<QToolButton *>(QStringLiteral("noteReplaceToggle"))) t->setChecked(false);
        m_replaceRow->hide();
    }

    if (m_reading) {
        m_editor->hide();
        m_preview->setColumnWidth(680);
        m_preview->show();
    } else {
        m_editor->show();
        m_preview->setColumnWidth(0);
        m_preview->setVisible(m_mode == EditorMode::Split);
        m_editor->setColumnWidth(m_mode == EditorMode::Split ? 560 : 680);
    }
    placeTopBar();
    alignHead();
}

void NotePane::showFind() {
    if (m_locked) return;
    if (!m_reading) m_editor->commitPendingCell();
    m_findBar->show();
    QTextEdit *view = activeTextView();
    if (view->textCursor().hasSelection()) {
        const QString selected = view->textCursor().selectedText();
        if (!selected.contains(QChar::ParagraphSeparator)) m_findText->setText(selected);
    }
    m_findText->setFocus();
    m_findText->selectAll();
    refreshFind();
}

void NotePane::showReplace() {
    showFind();
    if (m_reading || m_editor->isReadOnly()) return;
    findChild<QToolButton *>(QStringLiteral("noteReplaceToggle"))->setChecked(true);
    m_replaceRow->show();
    m_replaceOne->setEnabled(!m_editor->isReadOnly() && !m_reading);
    m_replaceAll->setEnabled(!m_editor->isReadOnly() && !m_reading);
}

void NotePane::replaceMatches(bool all) {
    if (m_reading || m_editor->isReadOnly() || m_findText->text().isEmpty()) return;
    m_editor->commitPendingCell();
    const QString query = m_findText->text();
    const QString replacement = m_replaceText->text();
    const Qt::CaseSensitivity sensitivity = m_findCase->isChecked() ? Qt::CaseSensitive : Qt::CaseInsensitive;
    if (!all) {
        if (m_editor->textCursor().selectedText().compare(query, sensitivity) != 0) findNext();
        QTextCursor match = m_editor->textCursor();
        if (match.selectedText().compare(query, sensitivity) != 0) return;
        match.beginEditBlock();
        match.insertText(replacement);
        match.endEditBlock();
        m_editor->setTextCursor(match);
        findNext();
    } else {
        QTextDocument::FindFlags flags;
        if (sensitivity == Qt::CaseSensitive) flags |= QTextDocument::FindCaseSensitively;
        QList<QTextCursor> matches;
        QTextCursor cursor(m_editor->document());
        while (true) {
            cursor = m_editor->document()->find(query, cursor, flags);
            if (cursor.isNull()) break;
            matches << cursor;
        }
        QTextCursor transaction(m_editor->document());
        transaction.beginEditBlock();
        for (auto it = matches.rbegin(); it != matches.rend(); ++it) it->insertText(replacement);
        transaction.endEditBlock();
    }
    refreshFind();
}

void NotePane::refreshFind() {
    if (!m_findBar->isVisible()) return;
    QTextEdit *view = activeTextView();
    const QString query = m_findText->text();
    QList<QTextEdit::ExtraSelection> marks;
    QTextDocument::FindFlags flags;
    if (m_findCase->isChecked()) flags |= QTextDocument::FindCaseSensitively;
    QTextCursor c(view->document());
    int count = 0;
    int current = 0;
    const QTextCursor selected = view->textCursor();
    while (!query.isEmpty()) {
        c = view->document()->find(query, c, flags);
        if (c.isNull()) break;
        ++count;
        const bool isCurrent = selected.hasSelection() && c.selectionStart() == selected.selectionStart() && c.selectionEnd() == selected.selectionEnd();
        if (isCurrent) current = count;
        // Bound decoration cost on large notes, while reporting the full count.
        if (marks.size() < 2000) {
            QTextEdit::ExtraSelection mark;
            mark.cursor = c;
            // The match the cursor is on stands out from the rest, also while
            // the focus is in the find field and the selection is drawn dim.
            mark.format.setBackground(Theme::alpha(Theme::t().warn, isCurrent ? 0.6 : 0.25));
            marks << mark;
        }
    }
    m_findCount->setText(query.isEmpty() ? QString()
                         : current ? QStringLiteral("%1 / %2").arg(current).arg(count)
                                   : QString::number(count));
    view->setExtraSelections(marks);
}

void NotePane::findNext(bool backward) {
    if (m_findText->text().isEmpty()) return;
    QTextEdit *view = activeTextView();
    QTextDocument::FindFlags flags;
    if (backward) flags |= QTextDocument::FindBackward;
    if (m_findCase->isChecked()) flags |= QTextDocument::FindCaseSensitively;
    QTextCursor found = view->document()->find(m_findText->text(), view->textCursor(), flags);
    if (found.isNull()) {
        QTextCursor edge(view->document());
        if (backward) edge.movePosition(QTextCursor::End);
        found = view->document()->find(m_findText->text(), edge, flags);
    }
    if (found.isNull()) return;
    view->setTextCursor(found);
    centerMatch();
    refreshFind();
}

// Brings the cursor to the middle of the editor, unless it already sits in the
// middle part: a match that is in view stays put, one at the edge, or far
// away, is moved to where it reads comfortably.
void NotePane::centerMatch() {
    QTextEdit *view = activeTextView();
    const QRect r = view->cursorRect();
    const int height = view->viewport()->height();
    const int y = r.center().y();
    if (y >= height * 0.35 && y <= height * 0.65) return;
    QScrollBar *bar = view->verticalScrollBar();
    bar->setValue(bar->value() + y - height / 2);
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
    if (m_mode != EditorMode::Split && !m_reading) return;
    RenderOptions opts;
    opts.resolveTitle = [this](const QString &t) { return resolveTitle(t); };
    if (m_engine) {
        opts.resourceKind = [this](const QString &id) { return m_engine->attachmentKind(id); };
        opts.resourceName = [this](const QString &id) { return m_engine->attachmentName(id); };
    }
    opts.interactiveTasks = !m_reading && !m_editor->isReadOnly();
    m_syncing = true;
    m_preview->setMarkdown(m_editor->markdown(), opts);
    m_syncing = false;
    m_scrollMap.clear();
    if (!m_reading && m_mode == EditorMode::Split) syncScroll(true);
}

void NotePane::placeTopBar() {
    m_topBar->adjustSize();
    m_topBar->move(width() - m_topBar->width() - 10, 16);
    m_topBar->raise();
}

void NotePane::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    placeTopBar();
    alignHead();
}

void NotePane::alignHead() {
    // The head spans the active pane's text column (centered in Live/Source/Read,
    // narrower in Split view, beside the outline).
    const QRect col = m_reading ? m_preview->textColumn() : m_editor->textColumn();
    const QPoint at = m_reading ? m_preview->mapTo(this, col.topLeft()) : m_editor->mapTo(this, col.topLeft());
    const int left = std::max(0, at.x());
    // Leave room for the floating actions when the column runs close to the edge.
    const int right = std::max({0, m_headHost->width() - left - col.width(), m_topBar->width() + 16});
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
    static const QRegularExpression image(QStringLiteral(R"(!\[[^\]\n]*\]\((?:ivy://res/([A-Za-z0-9_-]{8,64})|(https?://[^)\s]+)))"));
    for (QTextBlock b = src->begin(); b.isValid(); b = b.next()) {
        const auto *info = static_cast<BlockInfo *>(b.userData());
        if (info && (info->kind == BlockInfo::Code || info->kind == BlockInfo::Fence)) continue;
        const qreal y = sl->blockBoundingRect(b).top();
        if (info && info->kind == BlockInfo::Heading) srcHeads << y;
        else if (info && info->kind == BlockInfo::Table && info->tableRow == 0) srcTables << y;
        if (info && info->kind == BlockInfo::Image && !info->imageId.isEmpty()) {
            srcImages.append({info->imageId, y});
        } else if (b.text().contains(QLatin1String("![")))
            for (auto it = image.globalMatch(b.text()); it.hasNext();) {
                const auto m = it.next();
                srcImages.append({m.hasCaptured(1) ? m.captured(1) : m.captured(2), y});
            }
    }
    QSet<const QTextFrame *> seen;
    for (QTextBlock b = dst->begin(); b.isValid(); b = b.next()) {
        const qreal y = dl->blockBoundingRect(b).top();
        if (b.blockFormat().headingLevel() > 0) dstHeads << y;
        if (QTextFrame *f = QTextCursor(b).currentFrame(); f && f != dst->rootFrame() && qobject_cast<QTextTable *>(f) && !seen.contains(f) && qobject_cast<QTextTable *>(f)->cellAt(0, 0).format().hasProperty(QTextFormat::TableCellBottomBorder)) {
            seen.insert(f);
            dstTables << dl->frameBoundingRect(f).top();
        }
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextCharFormat cf = it.fragment().charFormat();
            if (!cf.isImageFormat()) continue;
            const QString name = cf.toImageFormat().name();
            if (name.startsWith(QLatin1String("ivy://res/"))) dstImages.append({name.mid(10), y});
            else if (name.startsWith(QLatin1String("http"))) dstImages.append({name, y});
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
    if (m_syncing || m_reading || m_mode != EditorMode::Split || !m_preview->isVisible()) return;
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
    const bool wasReading = m_reading;
    m_reading = false;
    m_mode = mode;
    m_editor->setMode(mode == EditorMode::Live ? MarkdownHighlighter::Live : MarkdownHighlighter::Source);
    const bool wasSplit = !wasReading && m_preview->isVisible();
    applyViewState();
    if (mode == EditorMode::Split) {
        if (!wasSplit) m_split->setSizes({1, 1});
        refreshPreview();
        if (!wasSplit) rise(m_preview, 220);
    }
    if (!m_id.isEmpty()) rise(m_editor, 160);
    if (wasReading) emit readingChanged(false);
    emit editorModeChanged(mode);
}

void NotePane::setReading(bool reading) {
    if (m_reading == reading) return;
    flush();
    m_editor->setExtraSelections({});
    m_preview->setExtraSelections({});
    m_reading = reading;
    applyViewState();
    if (m_reading) {
        refreshPreview();
        if (!m_id.isEmpty()) rise(m_preview, 160);
    } else {
        if (m_mode == EditorMode::Split) {
            m_split->setSizes({1, 1});
            refreshPreview();
        }
        if (!m_id.isEmpty()) rise(m_editor, 160);
    }
    if (m_findBar->isVisible()) refreshFind();
    emit readingChanged(m_reading);
}

void NotePane::prepareOpenMode(EditorMode mode, bool reading) {
    const bool modeChanged = m_mode != mode;
    const bool readChanged = m_reading != reading;
    m_mode = mode;
    m_reading = reading;
    m_editor->setMode(mode == EditorMode::Live ? MarkdownHighlighter::Live : MarkdownHighlighter::Source);
    if (modeChanged) emit editorModeChanged(m_mode);
    if (readChanged) emit readingChanged(m_reading);
}

void NotePane::setFontSize(int px) {
    m_editor->setFontSize(px);
    m_preview->setBaseSize(px);
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

void NotePane::focusEditor() {
    if (m_reading) m_preview->setFocus();
    else m_editor->setFocus();
}
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
            if (m_reading) {
                m_preview->scrollToAnchor(slug);
            } else {
                for (const md::Heading &h : md::outline(m_editor->markdown()))
                    if (md::slug(h.text) == slug) {
                        m_editor->scrollToLine(h.line);
                        break;
                    }
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
        if (m_reading) {
            m_preview->scrollToAnchor(slug);
        } else {
            for (const md::Heading &h : md::outline(m_editor->markdown()))
                if (md::slug(h.text) == slug) m_editor->scrollToLine(h.line);
        }
    }
    // Anything else (javascript:, file:, …) is ignored on purpose.
}

void NotePane::attach(const QByteArray &data, const QString &name, const QString &mime) {
    if (!m_engine || m_id.isEmpty()) return;
    if (m_locked) {
        Toaster::show(T("先解锁这篇笔记，再插入文件。"), true);
        return;
    }
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
        if (self->m_id == noteId && !self->m_locked) {
            self->m_editor->insertAttachmentMarkdown(md);
            if (self->m_mode == EditorMode::Split || self->m_reading) self->m_previewTimer.start();
        } else if (const Note *n = self->m_engine->note(noteId); n && !notelock::isEncrypted(n->body)) {
            NotePatch p;
            p.body = n->body + (n->body.endsWith(u'\n') || n->body.isEmpty() ? QString() : QStringLiteral("\n")) + md + u'\n';
            self->m_engine->updateNote(noteId, p);
        } else if (n) {
            // A sealed note that is not open cannot take text appended to it.
            Toaster::show(T("这篇笔记已加密，文件没有插入。请解锁后重新插入。"), true);
            return;
        }
        const bool isOffline = self->m_engine && self->m_engine->state().status == SyncState::Offline;
        if (f.local && isOffline && self->m_sealed) Toaster::show(T("离线中：加密笔记里的文件只在本机可见，联网后请重新插入。"), true);
        else if (f.local && isOffline) Toaster::show(T("离线中，文件将在联网后上传"));
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

void NotePane::showInfo() {
    if (!m_session || !m_engine || m_id.isEmpty() || !m_engine->note(m_id)) return;
    flush();
    NoteInfoDialog dialog(m_session, m_id, this);
    const QString id = m_id;
    connect(&dialog, &NoteInfoDialog::shareRequested, this, [this, id] { QTimer::singleShot(0, this, [this, id] { emit shareRequested(id); }); });
    connect(&dialog, &NoteInfoDialog::historyRequested, this, [this, id] { QTimer::singleShot(0, this, [this, id] { emit historyRequested(id); }); });
    dialog.exec();
}

void NotePane::showMoreMenu() {
    if (!m_engine || m_id.isEmpty()) return;
    const Note *n = m_engine->note(m_id);
    if (!n) return;
    QMenu menu(this);
    const QString id = m_id;
    menu.addAction(icons::themed("info"), T("笔记信息"), this, &NotePane::showInfo);
    if (!m_locked) {
        QAction *readAct = menu.addAction(icons::themed(m_reading ? "edit" : "book"),
                                          m_reading ? T("退出阅读模式") : T("阅读模式"),
                                          this, &NotePane::toggleReading);
        readAct->setCheckable(true);
        readAct->setChecked(m_reading);
        menu.addAction(icons::themed("diff"), T("对比差异 (Diff)"), this, [this, id] { emit diffRequested(id, m_conflictTargetId); });
    }
    // The three editor modes are rarely switched, so they live here.
    QMenu *modes = menu.addMenu(icons::themed("eye"), T("编辑模式"));
    auto *modeGroup = new QActionGroup(modes);
    const struct { EditorMode mode; const char *label; } entries[] = {{EditorMode::Live, "所见即所得"}, {EditorMode::Source, "源码"}, {EditorMode::Split, "双栏"}};
    for (const auto &entry : entries) {
        QAction *a = modes->addAction(T(entry.label));
        a->setCheckable(true);
        a->setChecked(!m_reading && m_mode == entry.mode);
        modeGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, mode = entry.mode] {
            setEditorMode(mode);
            focusEditor();
        });
    }
    menu.addSeparator();
    if (!n->trashed()) {
        menu.addAction(icons::themed("pin"), n->pinned ? T("取消置顶") : T("设为置顶"), this, [this, id, pinned = n->pinned] {
            NotePatch p;
            p.pinned = !pinned;
            m_engine->updateNote(id, p);
        });
        menu.addAction(icons::themed("move"), T("移动到…"), this, [this, id] { emit moveRequested(id); });
        menu.addAction(icons::themed("tag"), T("标签…"), this, &NotePane::pickTags);
        if (!m_locked) {
            menu.addAction(icons::themed("paperclip"), T("插入图片或文件…"), this, &NotePane::pickFiles);
            menu.addAction(icons::themed("outline"), T("大纲"), this, &NotePane::toggleOutline);
        }
        menu.addAction(icons::themed("copy"), T("复制笔记"), this, [this, id] { emit duplicateRequested(id); });
        if (m_session && m_session->isRemote()) menu.addAction(icons::themed("history"), T("历史版本"), this, [this, id] { emit historyRequested(id); });
        menu.addSeparator();
        if (m_sealed) {
            menu.addAction(icons::themed("lock"), T("立即锁定"), this, &NotePane::relock);
            menu.addAction(icons::themed("edit"), T("修改密码…"), this, [this] { protectNote(true); });
            menu.addAction(icons::themed("x"), T("移除密码…"), this, &NotePane::removeProtection);
            menu.addSeparator();
        } else if (!m_locked) {
            menu.addAction(icons::themed("lock"), T("设置密码…"), this, [this] { protectNote(false); });
            menu.addSeparator();
        }
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

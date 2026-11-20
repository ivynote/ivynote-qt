#include "ui/mainwindow.h"

#include "core/agenda.h"
#include "core/format.h"
#include "core/i18n.h"
#include "core/models.h"
#include "sync/api.h"
#include "sync/engine.h"
#include "ui/dialogs.h"
#include "ui/diffdialog.h"
#include "ui/editor.h"
#include "ui/historydialog.h"
#include "ui/icons.h"
#include "ui/notepane.h"
#include "ui/palette.h"
#include "ui/panels.h"
#include "ui/serverpanels.h"
#include "ui/settingsdialog.h"
#include "ui/shelf.h"
#include "ui/theme.h"
#include "ui/toast.h"
#include "ui/updater.h"
#include "ui/vaultdialog.h"
#include "ui/widgets.h"
#include "vault/session.h"
#include "vault/transfer.h"

#include <QPainter>
#include <QTextBlock>

#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QFileDialog>
#include <QGuiApplication>
#include <QInputDialog>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QSignalBlocker>
#include <QTabBar>
#include <QToolBar>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRegularExpression>
#include <QTimer>
#include <QDateTime>
#include <QFileInfo>
#include <QEventLoop>
#include <QKeyEvent>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QVBoxLayout>

namespace {

QString noteKey(Session *s) { return QStringLiteral("vault/%1/lastNote").arg(s->info().id); }

// The empty paper: the leaf, a line, the keys that matter.
QWidget *emptyState(QWidget *parent) {
    auto *w = new QWidget(parent);
    auto *col = new QVBoxLayout(w);
    col->addStretch(3);
    col->addWidget(new LeafWidget(64, 0.7, w), 0, Qt::AlignHCenter);
    col->addSpacing(14);
    auto *line = new QLabel(T("像常春藤一样，你的笔记彼此相连、四季常青。"), w);
    line->setFont(Theme::serif(16, QFont::Normal));
    line->setAlignment(Qt::AlignHCenter);
    line->setWordWrap(true);
    col->addWidget(line);
    col->addSpacing(10);
    auto *keys = mutedLabel(QStringLiteral("Ctrl+N  %1     Ctrl+K  %2     Ctrl+\\  %3").arg(T("新建笔记"), T("搜索"), T("收起书架")), w, 12);
    keys->setAlignment(Qt::AlignHCenter);
    col->addWidget(keys);
    col->addStretch(4);
    QObject::connect(Theme::instance(), &Theme::changed, line, [line] {
        QPalette p = line->palette();
        p.setColor(QPalette::WindowText, Theme::t().ink2);
        line->setPalette(p);
    });
    return w;
}

QString welcomeBody() {
    if (!i18n::isZh())
        return QStringLiteral(
            "Notes are saved on this device as you type — there is no save button.\n\n"
            "- [ ] Try **bold**, *italic*, `code` and ==highlight==\n"
            "- [ ] Link notes with [[Ideas]]; a missing title creates the note\n"
            "- [ ] Paste or drop an image right into the text\n\n"
            "> Ctrl+K searches, Ctrl+N starts a note, Ctrl+\\ hides the shelf.\n\n"
            "This vault lives only on this computer. To sync between devices, choose **Connect to an Ivynote server** from the vault menu at the top left; "
            "you can import this vault into it later.\n");
    return QStringLiteral(
        "笔记在输入时就保存在本机，没有保存按钮。\n\n"
        "- [ ] 试试 **粗体**、*斜体*、`代码` 和 ==高亮==\n"
        "- [ ] 用 [[灵感]] 链接笔记，不存在的标题点一下就会新建\n"
        "- [ ] 直接把图片粘贴或拖进正文\n\n"
        "> Ctrl+K 搜索，Ctrl+N 新建笔记，Ctrl+\\ 收起书架。\n\n"
        "这个笔记库只保存在这台电脑上。要在多台设备间同步，请在左上角的笔记库菜单中选择 **连接 Ivynote 服务器**，之后可以把这里的笔记导入过去。\n");
}

} // namespace

MainWindow::MainWindow() {
    setWindowTitle(T("常青笔记"));
    setWindowIcon(icons::appIcon());
    resize(1180, 760);
    setMinimumSize(640, 420);

    m_split = new Divider(Qt::Horizontal, this);
    m_split->setChildrenCollapsible(false);
    // The sidebar holds one of three pages, chosen on the rail at the left edge.
    m_side = new QStackedWidget(m_split);
    m_side->setMinimumWidth(220);
    m_shelf = new Shelf(m_side);
    m_agenda = new AgendaPanel(m_side);
    m_media = new MediaPanel(m_side);
    m_side->addWidget(m_shelf);
    m_side->addWidget(m_agenda);
    m_side->addWidget(m_media);
    auto *paper = new Paper(m_split);
    auto *paperCol = new QVBoxLayout(paper);
    paperCol->setContentsMargins(0, 0, 0, 0);
    m_stack = new QStackedWidget(paper);
    m_pane = new NotePane(m_stack);
    m_empty = emptyState(m_stack);
    m_stack->addWidget(m_empty);
    m_stack->addWidget(m_pane);
    m_tabs = new QTabBar(paper);
    m_tabs->setDocumentMode(true);
    m_tabs->setMovable(true);
    m_tabs->setExpanding(false);
    m_tabs->setTabsClosable(true);
    m_tabs->setDrawBase(false);
    m_tabs->setElideMode(Qt::ElideRight);
    m_tabs->hide();
    paperCol->addWidget(m_tabs);
    paperCol->addWidget(m_stack);
    connect(m_tabs, &QTabBar::currentChanged, this, [this](int index) {
        openNote(m_tabs->tabData(index).toString());
    });
    connect(m_tabs, &QTabBar::tabCloseRequested, this, [this](int index) {
        const bool active = m_tabs->currentIndex() == index;
        const QSignalBlocker blocker(m_tabs);
        m_tabs->removeTab(index);
        m_tabs->setVisible(m_tabs->count() > 1);
        if (active) openNote(m_tabs->tabData(m_tabs->currentIndex()).toString());
    });
    m_split->addWidget(m_side);
    m_split->addWidget(paper);
    m_split->setStretchFactor(1, 1);
    m_split->setSizes({296, 880});
    setCentralWidget(m_split);
    Toaster::install(this);

    statusBar()->setSizeGripEnabled(false);
    m_sync = new SyncIndicator(this);
    statusBar()->addWidget(m_sync);
    connect(m_sync, &QToolButton::clicked, this, [this] {
        if (!m_session || !m_session->isRemote()) return;
        if (!m_session->signedIn() || m_session->engine()->paused()) return signIn();
        m_session->wake();
    });
    connect(m_agenda, &AgendaPanel::taskActivated, this, &MainWindow::openTask);
    connect(m_agenda, &AgendaPanel::taskToggled, this, &MainWindow::toggleAgendaTask);
    connect(m_agenda, &AgendaPanel::taskDueChanged, this, &MainWindow::rescheduleAgendaTask);
    connect(m_agenda, &AgendaPanel::badgeChanged, this, [this](int count, bool overdue) {
        if (m_agendaBadgeCount == count && m_agendaBadgeOverdue == overdue) return;
        m_agendaBadgeCount = count;
        m_agendaBadgeOverdue = overdue;
        refreshRailIcons();
    });

    connect(m_shelf, &Shelf::openNote, this, &MainWindow::openNote);
    connect(m_shelf, &Shelf::newNote, this, &MainWindow::newNote);
    connect(m_shelf, &Shelf::moveNotes, this, &MainWindow::moveNotes);
    connect(m_shelf, &Shelf::searchRequested, this, [this] { showSearch(); });
    connect(m_shelf, &Shelf::vaultMenuRequested, this, &MainWindow::showVaultMenu);
    connect(m_shelf, &Shelf::emptyTrashRequested, this, &MainWindow::emptyTrash);
    connect(m_shelf, &Shelf::diffRequested, this, &MainWindow::openDiff);
    connect(m_pane, &NotePane::openNote, this, &MainWindow::openNote);
    connect(m_pane, &NotePane::moveRequested, this, &MainWindow::moveNote);
    connect(m_pane, &NotePane::duplicateRequested, this, &MainWindow::duplicateNote);
    connect(m_pane, &NotePane::diffRequested, this, &MainWindow::openDiff);
    connect(m_pane, &NotePane::historyRequested, this, [this](const QString &id) {
        if (!m_session) return;
        m_pane->flush();
        HistoryDialog dlg(m_session, id, this);
        dlg.exec();
    });
    connect(m_pane, &NotePane::shareRequested, this, [this](const QString &id) {
        if (!m_session || !m_session->isRemote()) return;
        if (const Note *n = m_session->engine()->note(id); n && notelock::isEncrypted(n->body)) {
            Toaster::show(T("加密的笔记不能公开分享。"), true);
            return;
        }
        m_pane->flush();
        m_session->engine()->sync();
        ShareDialog dlg(m_session, id, this);
        dlg.exec();
    });
    connect(m_pane, &NotePane::editorModeChanged, this, [this](EditorMode mode) {
        if (m_modeGroup) m_modeGroup->actions().value(int(mode))->setChecked(true);
    });
    connect(m_pane, &NotePane::readingChanged, this, [this](bool reading) {
        if (m_readAction) {
            const QSignalBlocker blocker(m_readAction);
            m_readAction->setChecked(reading);
        }
    });
    connect(Prefs::instance(), &Prefs::changed, this, &MainWindow::applyPrefs);
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState s) {
        if (!m_session) return;
        if (s == Qt::ApplicationActive) m_session->wake();
        else {
            // Leaving the foreground: everything typed is on disk.
            m_pane->flush();
            m_session->engine()->persistNow();
        }
    });

    buildMenus();
    applyPrefs();
    QSettings s;
    restoreGeometry(s.value(QStringLiteral("window/geometry")).toByteArray());
    const QList<int> sizes = {s.value(QStringLiteral("window/shelf"), 296).toInt(), 900};
    m_split->setSizes(sizes);
    showPage(Page(std::clamp(s.value(QStringLiteral("window/page"), 0).toInt(), 0, 2)), false);
    if (s.value(QStringLiteral("window/shelfCollapsed"), false).toBool()) toggleShelf();
}

MainWindow::~MainWindow() { closeVault(); }

void MainWindow::start() {
    QTimer::singleShot(1500, this, [this] { updater::check(this, false); });
    auto *updates = new QTimer(this);
    connect(updates, &QTimer::timeout, this, [this] { updater::check(this, false); });
    updates->start(6 * 60 * 60 * 1000);
    QString id = vaults::lastOpened();
    if (vaults::find(id).id.isEmpty()) id = vaults::list().isEmpty() ? QString() : vaults::list().first().id;
    if (id.isEmpty()) {
        // First run: a local vault, ready to write in.
        VaultInfo v;
        v.name = T("我的笔记");
        v.kind = VaultInfo::Local;
        QString err;
        if (vaults::create(v, &err) && openVault(v.id)) {
            NoteInit welcome;
            welcome.title = T("欢迎使用常青笔记");
            welcome.body = welcomeBody();
            openNote(m_session->engine()->createNote(welcome).id);
            m_session->engine()->persistNow();
        } else {
            QMessageBox::critical(this, T("常青笔记"), err);
        }
        return;
    }
    if (!openVault(id)) {
        // Fall back to any vault that opens, or offer to add one.
        for (const VaultInfo &v : vaults::list())
            if (v.id != id && openVault(v.id)) return;
        addVault();
    }
}

bool MainWindow::openVault(const QString &id) {
    const VaultInfo info = vaults::find(id);
    if (info.id.isEmpty()) return false;
    closeVault();
    Session::OpenError err = Session::OpenError::None;
    QString message;
    Session *s = Session::open(info, {}, &err, &message, this);
    bool wrong = false;
    while (!s && (err == Session::OpenError::MissingKey || err == Session::OpenError::WrongKey)) {
        // The keychain lost the key: only the recovery key opens the vault now.
        const QByteArray key = dialogs::askRecoveryKey(this, info.name, wrong);
        if (key.isEmpty()) return false;
        s = Session::open(info, key, &err, &message, this);
        wrong = true;
    }
    if (!s) {
        QMessageBox::critical(this, T("无法打开笔记库"), message);
        return false;
    }
    m_session = s;
    vaults::setLastOpened(id);
    m_shelf->setSession(s);
    m_agenda->setSession(s);
    m_media->setSession(s);
    m_sync->setSession(s);
    m_pane->setSession(s);
    applyOpenMode(Prefs::instance()->editorMode());
    m_pane->setFontSize(Prefs::instance()->fontSize());
    connect(s->engine(), &SyncEngine::conflict, this, [this](const QString &origId, const QString &copyId) {
        Toaster::show(T("同一处被两台设备修改，另一份内容已存为冲突副本"), false, T("对比差异"), [this, origId, copyId] { openDiff(origId, copyId); });
    });
    connect(s->engine(), &SyncEngine::rejected, this, [](const QString &, const QString &, const QString &error) {
        if (error.contains(QLatin1String("hlc"))) Toaster::show(T("服务器拒绝了更改：请检查这台电脑的系统时间，校准后重启应用"), true);
        else Toaster::show(T("服务器拒绝了一条更改，它会保留在本机") + QStringLiteral("（") + error + QStringLiteral("）"), true);
    });
    connect(s->engine(), &SyncEngine::uploadFailed, this, [](const QString &name, const QString &error) {
        Toaster::show((i18n::isZh() ? QStringLiteral("%1 无法上传：%2") : QStringLiteral("%1 could not upload: %2")).arg(name, error), true);
    });
    m_signInAsked = false;
    connect(s, &Session::needsSignIn, this, [this] {
        if (m_signInAsked) return;
        m_signInAsked = true;
        QTimer::singleShot(0, this, &MainWindow::signIn);
    });
    connect(s, &Session::userChanged, this, &MainWindow::updateTitle);
    if (s->unreadableRecords() > 0)
        Toaster::show(T("有部分本机记录无法读取，已跳过"), true);
    connect(s->engine(), &SyncEngine::dataChanged, this, [this] {
        refreshNoteTabs();
        const auto id = m_pane->noteId();
        const auto *note = m_session->engine()->note(id);
        if (!id.isEmpty() && (!note || note->deleted))
            openNote(m_tabs->tabData(m_tabs->currentIndex()).toString());
    });
    auto *reminders = new QTimer(s);
    connect(reminders, &QTimer::timeout, s, [this, s, previous = QDateTime::currentDateTime()]() mutable {
        if (m_session != s) return;
        const auto now = QDateTime::currentDateTime();
        const auto from = previous;
        previous = now;
        for (const auto &task : agendaTasks(s->engine()->notes())) {
            if (task.due <= from || task.due > now) continue;
            Toaster::show(task.title + QStringLiteral(" · ") + task.text, false, T("查看"), [this, vault = s->info().id, id = task.noteId] {
                if (m_session && m_session->info().id == vault) openNote(id);
            });
        }
    });
    reminders->start(30000);
    s->start();
    syncPrefsFromServer();
    rebuildVaultMenu();
    updateTitle();
    for (QAction *a : std::as_const(m_remoteActions)) a->setVisible(s->isRemote());
    const QString last = QSettings().value(noteKey(s)).toString();
    if (s->engine()->note(last)) {
        openNote(last);
    } else {
        openNote(QString());
        // A fresh device has the note only after its first sync.
        if (s->isRemote() && !last.isEmpty()) {
            connect(s->engine(), &SyncEngine::roundFinished, this, [this, last](bool) {
                if (m_session && m_pane->noteId().isEmpty() && m_session->engine()->note(last)) openNote(last);
            }, Qt::SingleShotConnection);
        }
    }
    return true;
}

void MainWindow::closeVault() {
    if (!m_session) return;
    m_pane->flush();
    dropEmptyDraft(QString());
    m_shelf->setSession(nullptr);
    m_agenda->setSession(nullptr);
    m_media->setSession(nullptr);
    m_sync->setSession(nullptr);
    m_pane->setSession(nullptr);
    m_agendaBadgeCount = 0;
    m_agendaBadgeOverdue = false;
    refreshRailIcons();
    {
        const QSignalBlocker blocker(m_tabs);
        while (m_tabs->count()) m_tabs->removeTab(0);
        m_tabs->hide();
    }
    Session *s = m_session;
    m_session = nullptr;
    s->close();
    s->deleteLater();
}

void MainWindow::updateTitle() {
    setWindowTitle(m_session ? m_session->info().name + QStringLiteral(" — ") + T("常青笔记") : T("常青笔记"));
}

void MainWindow::syncPrefsFromServer() {
    if (!m_session || !m_session->isRemote() || !m_session->signedIn()) return;
    QPointer<MainWindow> self(this);
    m_session->api()->get(QStringLiteral("/me/prefs"), m_session, [self](const ApiReply &r) {
        if (!self || !r.ok) return;
        const QJsonObject o = r.json.toObject();
        const QString mode = o.value(QLatin1String("editorMode")).toString();
        const int size = o.value(QLatin1String("fontSize")).toInt();
        Prefs *p = Prefs::instance();
        const QStringList modes{QStringLiteral("live"), QStringLiteral("source"), QStringLiteral("split"), QStringLiteral("read")};
        if (modes.contains(mode) && modes.indexOf(mode) != p->editorMode()) p->setEditorMode(int(modes.indexOf(mode)));
        if (size >= 13 && size <= 22 && size != p->fontSize()) p->setFontSize(size);
        if (o.contains(QLatin1String("defaultNoteFolder")) && o.value(QLatin1String("defaultNoteFolder")).isString()) {
            const QString folder = o.value(QLatin1String("defaultNoteFolder")).toString();
            if (folder != p->defaultNoteFolder()) p->setDefaultNoteFolder(folder);
        }
        if (o.contains(QLatin1String("defaultNoteTitle")) && o.value(QLatin1String("defaultNoteTitle")).isString()) {
            const QString title = o.value(QLatin1String("defaultNoteTitle")).toString();
            if (title != p->defaultNoteTitle()) p->setDefaultNoteTitle(title);
        }
    });
}

void MainWindow::applyOpenMode(int mode) {
    if (mode == int(OpenMode::Read)) {
        if (m_pane->editorMode() != EditorMode::Live) m_pane->setEditorMode(EditorMode::Live);
        m_pane->setReading(true);
    } else {
        const auto em = EditorMode(std::clamp(mode, 0, 2));
        if (m_pane->editorMode() != em || m_pane->isReading()) m_pane->setEditorMode(em);
    }
}

void MainWindow::applyPrefs() {
    Prefs *p = Prefs::instance();
    Theme::setReducedMotion(p->reduceMotion());
    if (Theme::instance()->mode() != Theme::Mode(p->theme())) Theme::instance()->setMode(Theme::Mode(p->theme()));
    i18n::setLang(i18n::Lang(p->lang()));
    m_pane->setFontSize(p->fontSize());
    if (p->editorMode() != m_appliedOpenMode) {
        m_appliedOpenMode = p->editorMode();
        applyOpenMode(m_appliedOpenMode);
    }
}

// --- menus ---

void MainWindow::buildMenus() {
    QMenuBar *bar = menuBar();
    QMenu *file = bar->addMenu(T("文件(&F)"));
    file->addAction(T("新建笔记"), QKeySequence::New, this, [this] { newNote(QString()); });
    file->addAction(T("新建笔记本…"), this, [this] {
        if (!m_session) return;
        bool ok = false;
        const QString name = QInputDialog::getText(this, T("新建笔记本"), T("名称"), QLineEdit::Normal, QString(), &ok).trimmed();
        if (ok && !name.isEmpty()) m_session->engine()->createFolder(name.left(100));
    });
    file->addSeparator();
    m_vaultMenu = file->addMenu(T("切换笔记库"));
    file->addAction(T("新建或接入笔记库…"), this, [this] { addVault(); });
    file->addSeparator();
    QMenu *import = file->addMenu(T("导入"));
    const auto addImport = [this, import](const QString &label, transfer::MarkdownSource source) {
        import->addAction(label, this, [this, label, source] {
            if (!m_session) return;
            const QString folder = QFileDialog::getExistingDirectory(this, label);
            if (folder.isEmpty()) return;
            runJob(transfer::importMarkdown(m_session, {folder}, QString(), this, source), T("正在导入…"));
        });
    };
    import->addAction(T("导入 Markdown…"), this, &MainWindow::importMarkdown);
    addImport(T("导入 Joplin Markdown 文件夹…"), transfer::MarkdownSource::Joplin);
    addImport(T("导入 Obsidian 笔记库…"), transfer::MarkdownSource::Obsidian);
    import->addSeparator();
    import->addAction(T("从其他笔记库导入…"), this, &MainWindow::importVault);
    file->addAction(T("导出为 Markdown…"), this, &MainWindow::exportMarkdown);
    file->addSeparator();
    file->addAction(T("设置…"), QKeySequence(QStringLiteral("Ctrl+,")), this, [this] { showSettings(); });
    file->addSeparator();
    file->addAction(T("退出"), QKeySequence::Quit, this, &QWidget::close);

    QMenu *edit = bar->addMenu(T("编辑(&E)"));
    auto fmtAction = [this, edit](const QString &label, const QString &keys, std::function<void(MarkdownEditor *)> f) {
        QAction *a = edit->addAction(label + u'\t' + keys);
        connect(a, &QAction::triggered, this, [this, f] {
            if (m_pane->noteId().isEmpty() || m_pane->editor()->isReadOnly()) return;
            m_pane->editor()->setFocus();
            f(m_pane->editor());
        });
        m_noteActions << a;
    };
    edit->addAction(T("搜索…"), QKeySequence(QStringLiteral("Ctrl+K")), this, [this] { showSearch(); });
    edit->addAction(T("命令面板…"), QKeySequence(QStringLiteral("Ctrl+Shift+P")), this, [this] { showSearch(QStringLiteral(">")); });
    edit->addAction(T("在笔记中查找") + QStringLiteral("\tCtrl+F"), m_pane, &NotePane::showFind);
    edit->addAction(T("查找与替换") + QStringLiteral("\tCtrl+H"), m_pane, &NotePane::showReplace);
    edit->addAction(T("插入") + QStringLiteral("\tCtrl+/"), m_pane->editor(), &MarkdownEditor::showInsertMenu);
    edit->addSeparator();
    fmtAction(T("粗体"), QStringLiteral("Ctrl+B"), [](MarkdownEditor *e) { e->toggleFormat(QStringLiteral("**")); });
    fmtAction(T("斜体"), QStringLiteral("Ctrl+I"), [](MarkdownEditor *e) { e->toggleFormat(QStringLiteral("*")); });
    fmtAction(T("行内代码"), QStringLiteral("Ctrl+E"), [](MarkdownEditor *e) { e->toggleFormat(QStringLiteral("`")); });
    fmtAction(T("删除线"), QStringLiteral("Ctrl+Shift+X"), [](MarkdownEditor *e) { e->toggleFormat(QStringLiteral("~~")); });
    fmtAction(T("高亮"), QString(), [](MarkdownEditor *e) { e->toggleFormat(QStringLiteral("==")); });
    fmtAction(T("链接到笔记 [[ ]]"), QStringLiteral("Ctrl+Shift+K"), [](MarkdownEditor *e) {
        QKeyEvent k(QEvent::KeyPress, Qt::Key_K, Qt::ControlModifier | Qt::ShiftModifier);
        QApplication::sendEvent(e, &k);
    });
    edit->addSeparator();
    for (int level = 1; level <= 3; ++level)
        fmtAction((i18n::isZh() ? QStringLiteral("标题 %1") : QStringLiteral("Heading %1")).arg(level), QStringLiteral("Ctrl+Alt+%1").arg(level),
                  [level](MarkdownEditor *e) { e->setHeading(level); });
    fmtAction(T("有序列表"), QStringLiteral("Ctrl+Shift+7"), [](MarkdownEditor *e) { e->toggleList(QStringLiteral("1.")); });
    fmtAction(T("无序列表"), QStringLiteral("Ctrl+Shift+8"), [](MarkdownEditor *e) { e->toggleList(QStringLiteral("-")); });
    fmtAction(T("待办"), QStringLiteral("Ctrl+Shift+9"), [](MarkdownEditor *e) { e->toggleList(QStringLiteral("- [ ]")); });
    edit->addSeparator();
    QAction *attach = edit->addAction(T("插入图片或文件…"), this, [this] { m_pane->pickFiles(); });
    m_noteActions << attach;

    QMenu *view = bar->addMenu(T("视图(&V)"));
    m_modeGroup = new QActionGroup(this);
    const QList<QPair<EditorMode, const char *>> modes{{EditorMode::Live, "所见即所得"}, {EditorMode::Source, "源码"}, {EditorMode::Split, "双栏"}};
    const int initialPref = Prefs::instance()->editorMode();
    for (const auto &[mode, label] : modes) {
        QAction *a = view->addAction(T(label));
        a->setCheckable(true);
        a->setChecked(int(mode) == (initialPref == int(OpenMode::Read) ? int(EditorMode::Live) : initialPref));
        m_modeGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, m = mode] {
            m_pane->setEditorMode(m);
        });
    }
    m_readAction = view->addAction(T("阅读模式"), QKeySequence(QStringLiteral("Ctrl+Shift+E")), this, [this](bool checked) {
        if (!m_pane->noteId().isEmpty()) m_pane->setReading(checked);
    });
    m_readAction->setCheckable(true);
    m_readAction->setChecked(initialPref == int(OpenMode::Read));
    m_noteActions << m_readAction;
    view->addSeparator();
    QAction *outline = view->addAction(T("大纲"), this, [this] { m_pane->toggleOutline(); });
    m_noteActions << outline;
    view->addSeparator();
    QMenu *theme = view->addMenu(T("主题"));
    auto *themeGroup = new QActionGroup(this);
    const QList<QPair<int, const char *>> themes{{0, "跟随系统"}, {1, "浅色"}, {2, "深色"}};
    for (const auto &[v, label] : themes) {
        QAction *a = theme->addAction(T(label));
        a->setCheckable(true);
        a->setChecked(Prefs::instance()->theme() == v);
        themeGroup->addAction(a);
        connect(a, &QAction::triggered, this, [v = v] { Prefs::instance()->setTheme(v); });
    }
    view->addAction(T("放大字号"), QKeySequence::ZoomIn, this, [] { Prefs::instance()->setFontSize(Prefs::instance()->fontSize() + 1); });
    view->addAction(T("缩小字号"), QKeySequence::ZoomOut, this, [] { Prefs::instance()->setFontSize(Prefs::instance()->fontSize() - 1); });

    QMenu *note = bar->addMenu(T("笔记(&N)"));
    auto noteAction = [this, note](const QString &label, std::function<void(const QString &)> f, bool remote = false) {
        QAction *a = note->addAction(label);
        connect(a, &QAction::triggered, this, [this, f] {
            if (!m_pane->noteId().isEmpty()) f(m_pane->noteId());
        });
        m_noteActions << a;
        if (remote) m_remoteActions << a;
        return a;
    };
    noteAction(T("置顶 / 取消置顶"), [this](const QString &id) {
        const Note *n = m_session->engine()->note(id);
        if (!n) return;
        NotePatch p;
        p.pinned = !n->pinned;
        m_session->engine()->updateNote(id, p);
    });
    noteAction(T("移动到…"), [this](const QString &id) { moveNote(id); });
    noteAction(T("标签…"), [this](const QString &) { m_pane->pickTags(); });
    noteAction(T("复制笔记"), [this](const QString &id) { duplicateNote(id); });
    note->addSeparator();
    noteAction(T("笔记信息…"), [this](const QString &) { m_pane->showInfo(); });
    noteAction(T("对比差异 (Diff)…"), [this](const QString &id) { openDiff(id); });
    noteAction(T("历史版本…"), [this](const QString &id) { emit m_pane->historyRequested(id); }, true);
    noteAction(T("分享…"), [this](const QString &id) { emit m_pane->shareRequested(id); }, true);
    note->addSeparator();
    noteAction(T("移到回收站"), [this](const QString &id) {
        m_pane->flush();
        NotePatch p;
        p.trashed = true;
        m_session->engine()->updateNote(id, p);
    });
    note->addSeparator();
    m_noteActions << note->addAction(T("设置日期与重复") + QStringLiteral("\tCtrl+Shift+D"), this, [this] {
        if (!m_session || m_pane->noteId().isEmpty() || m_pane->editor()->isReadOnly()) return;
        if (m_pane->isReading()) m_pane->setReading(false);
        m_pane->focusEditor();
        m_pane->editor()->openDuePicker();
    });

    // The rail at the left edge: where you are, not what you can do. Actions
    // live in the menus; clicking the page you are on folds the sidebar.
    m_rail = new QToolBar(T("导航栏"), this);
    m_rail->setObjectName(QStringLiteral("navigationDock"));
    m_rail->setOrientation(Qt::Vertical);
    m_rail->setMovable(false);
    m_rail->setFloatable(false);
    m_rail->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_rail->setIconSize(QSize(20, 20));
    m_rail->setContextMenuPolicy(Qt::PreventContextMenu);
    addToolBar(Qt::LeftToolBarArea, m_rail);
    const struct { const char *icon; const char *label; Page page; } pages[] = {
        {"note", "笔记", NotesPage}, {"calendar", "日程", AgendaPage}, {"image", "媒体库", MediaPage}};
    int number = 1;
    for (const auto &entry : pages) {
        auto *a = m_rail->addAction(T(entry.label));
        a->setCheckable(true);
        a->setShortcut(QKeySequence(QStringLiteral("Ctrl+%1").arg(number)));
        a->setToolTip(T(entry.label) + QStringLiteral(" (Ctrl+%1)").arg(number++));
        a->setData(QString::fromLatin1(entry.icon));
        connect(a, &QAction::triggered, this, [this, page = entry.page] {
            if (!m_shelfCollapsed && m_page == page) toggleShelf();
            else showPage(page, false);
        });
        m_railPages << a;
        view->insertAction(outline, a);
    }
    view->insertSeparator(outline);
    QAction *fold = new QAction(T("收起书架"), view);
    fold->setShortcut(QKeySequence(QStringLiteral("Ctrl+\\")));
    connect(fold, &QAction::triggered, this, &MainWindow::toggleShelf);
    view->insertAction(outline, fold);
    auto *spacer = new QWidget(m_rail);
    spacer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    m_rail->addWidget(spacer);
    m_railSettings = m_rail->addAction(T("设置"), this, [this] { showSettings(); });
    m_railSettings->setToolTip(T("设置") + QStringLiteral(" (Ctrl+,)"));
    m_railSettings->setData(QStringLiteral("settings"));
    refreshRailIcons();
    connect(Theme::instance(), &Theme::changed, this, &MainWindow::refreshRailIcons);

    // Everything that needs a server account.
    QMenu *server = bar->addMenu(T("服务器(&S)"));
    m_remoteActions << server->menuAction();
    QAction *syncNow = server->addAction(T("立即同步"), this, [this] {
        if (m_session) m_session->wake();
    });
    syncNow->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));
    m_remoteActions << syncNow;
    server->addSeparator();
    const auto remotePanel = [this, server](const QString &name, void (*open)(QWidget *, Session *)) {
        server->addAction(name, this, [this, open] {
            if (!m_session || !m_session->isRemote()) return;
            if (!m_session->signedIn()) signIn();
            if (m_session && m_session->signedIn()) open(this, m_session);
        });
    };
    remotePanel(T("日历订阅"), serverpanels::calendar);
    remotePanel(T("发布到博客"), serverpanels::publish);

    QMenu *help = bar->addMenu(T("帮助(&H)"));
    help->addAction(T("检查更新"), this, [this] { updater::check(this, true); });
    help->addAction(T("关于常青笔记"), this, [this] { showSettings(AboutPage); });
}

void MainWindow::rebuildVaultMenu() {
    if (!m_vaultMenu) return;
    m_vaultMenu->clear();
    for (const VaultInfo &v : vaults::list()) {
        QAction *a = m_vaultMenu->addAction(icons::themed(v.isRemote() ? QStringLiteral("cloud") : QStringLiteral("database")), v.name);
        a->setCheckable(true);
        a->setChecked(m_session && m_session->info().id == v.id);
        const QString id = v.id;
        connect(a, &QAction::triggered, this, [this, id] {
            if (m_session && m_session->info().id == id) return;
            const QString current = m_session ? m_session->info().id : QString();
            if (!openVault(id) && !current.isEmpty()) openVault(current);
        });
    }
}

void MainWindow::showVaultMenu(const QPoint &global) {
    QMenu menu(this);
    for (const VaultInfo &v : vaults::list()) {
        QAction *a = menu.addAction(icons::themed(v.isRemote() ? QStringLiteral("cloud") : QStringLiteral("database")),
                                    v.name + (v.isRemote() ? QString() : QStringLiteral("  ·  ") + T("本地")));
        a->setCheckable(true);
        a->setChecked(m_session && m_session->info().id == v.id);
        const QString id = v.id;
        connect(a, &QAction::triggered, this, [this, id] {
            if (m_session && m_session->info().id == id) return;
            const QString current = m_session ? m_session->info().id : QString();
            if (!openVault(id) && !current.isEmpty()) openVault(current);
        });
    }
    menu.addSeparator();
    menu.addAction(icons::themed("plus"), T("新建本地笔记库…"), this, [this] { addVault(false); });
    menu.addAction(icons::themed("cloud"), T("连接 Ivynote 服务器…"), this, [this] { addVault(true); });
    menu.addSeparator();
    menu.addAction(icons::themed("settings"), T("笔记库设置…"), this, [this] { showSettings(VaultPage); });
    menu.exec(global);
}

void MainWindow::addVault(bool server) {
    VaultDialog dlg(this, server);
    if (dlg.exec() != QDialog::Accepted || dlg.vaultId().isEmpty()) {
        if (!m_session && !vaults::list().isEmpty()) openVault(vaults::list().first().id);
        return;
    }
    openVault(dlg.vaultId());
}

void MainWindow::signIn() {
    if (!m_session || !m_session->isRemote() || m_signInOpen) return;
    m_signInOpen = true;
    SignInDialog dlg(m_session->info(), this);
    const bool ok = dlg.exec() == QDialog::Accepted;
    m_signInOpen = false;
    if (!ok || !m_session) return;
    QString err;
    if (m_session->applySignIn(dlg.token(), dlg.user(), &err)) {
        Toaster::show(T("已登录，正在继续同步"));
        syncPrefsFromServer();
    }
}

// --- notes ---

void MainWindow::openNote(const QString &id) {
    openNoteInMode(id, false);
}

void MainWindow::openNoteInMode(const QString &id, bool forEditing) {
    if (!m_session) return;
    const Note *note = m_session->engine()->note(id);
    const bool has = note && !note->deleted;
    const bool switching = has && m_pane->noteId() != id;
    if (switching) {
        m_pane->flush();
        const int pref = Prefs::instance()->editorMode();
        if (pref == int(OpenMode::Read) && !forEditing) {
            m_pane->prepareOpenMode(EditorMode::Live, true);
        } else {
            const auto em = pref == int(OpenMode::Read) ? EditorMode::Live : EditorMode(std::clamp(pref, 0, 2));
            m_pane->prepareOpenMode(em, false);
        }
    } else if (has && forEditing && m_pane->isReading()) {
        m_pane->setReading(false);
    }
    if (m_pane->noteId() != id || !has) m_pane->setNote(has ? id : QString());
    QWidget *page = has ? static_cast<QWidget *>(m_pane) : m_empty;
    if (m_stack->currentWidget() != page) {
        m_stack->setCurrentWidget(page);
        rise(page, 200);
    }
    // Leaving a new note that was never written in: it was never wanted.
    dropEmptyDraft(has ? id : QString());
    m_shelf->setCurrentNote(has ? id : QString());
    for (QAction *a : std::as_const(m_noteActions)) a->setEnabled(has);
    if (has) {
        const QSignalBlocker blocker(m_tabs);
        int index = 0;
        while (index < m_tabs->count() && m_tabs->tabData(index).toString() != id) ++index;
        if (index == m_tabs->count()) {
            index = m_tabs->addTab(QString());
            m_tabs->setTabData(index, id);
        }
        m_tabs->setCurrentIndex(index);
        QSettings().setValue(noteKey(m_session), id);
    }
    refreshNoteTabs();
}

void MainWindow::refreshNoteTabs() {
    if (!m_session) return;
    const QSignalBlocker blocker(m_tabs);
    for (int index = m_tabs->count() - 1; index >= 0; --index) {
        const auto *note = m_session->engine()->note(m_tabs->tabData(index).toString());
        if (!note || note->deleted) {
            m_tabs->removeTab(index);
            continue;
        }
        const QString title = note->title.trimmed().isEmpty() ? T("无标题") : note->title;
        QString label = title;
        m_tabs->setTabText(index, label.replace(u'&', QStringLiteral("&&")));
        m_tabs->setTabToolTip(index, title);
    }
    m_tabs->setVisible(m_tabs->count() > 1);
}

void MainWindow::openTask(const QString &noteId, int line) {
    if (!m_session) return;
    m_pane->flush();
    openNoteInMode(noteId, true);
    const auto block = m_pane->editor()->document()->findBlockByNumber(line);
    if (block.isValid()) {
        m_pane->editor()->setTextCursor(QTextCursor(block));
        m_pane->focusEditor();
    }
}

namespace {
int resolveTaskLine(const QString &body, int line, const QString &rawLine) {
    const QStringList lines = body.split(u'\n');
    if (line >= 0 && line < lines.size() && lines[line] == rawLine) return line;
    if (!rawLine.isEmpty()) {
        const int idx = lines.indexOf(rawLine);
        if (idx >= 0) return idx;
    }
    return -1;
}
} // namespace

void MainWindow::toggleAgendaTask(const QString &noteId, int line, const QString &rawLine) {
    if (!m_session) return;
    if (m_pane->noteId() == noteId) m_pane->flush();
    const Note *n = m_session->engine()->note(noteId);
    if (!n || n->deleted || notelock::isEncrypted(n->body)) return;
    const int target = resolveTaskLine(n->body, line, rawLine);
    if (target < 0) return;
    const QString nextBody = tasks::toggleTaskLine(n->body, target);
    if (nextBody == n->body) return;
    NotePatch p;
    p.body = nextBody;
    m_session->engine()->updateNote(noteId, p);
    if (m_pane->noteId() == noteId) m_pane->syncBodyFromEngine();
}

void MainWindow::rescheduleAgendaTask(const QString &noteId, int line, const QString &rawLine,
                                      const std::optional<tasks::Due> &due, const QString &repeat) {
    if (!m_session) return;
    if (m_pane->noteId() == noteId) m_pane->flush();
    const Note *n = m_session->engine()->note(noteId);
    if (!n || n->deleted || notelock::isEncrypted(n->body)) return;
    const int target = resolveTaskLine(n->body, line, rawLine);
    if (target < 0) return;
    QStringList lines = n->body.split(u'\n');
    lines[target] = tasks::setTaskDue(lines[target], due, repeat);
    const QString nextBody = lines.join(u'\n');
    if (nextBody == n->body) return;
    NotePatch p;
    p.body = nextBody;
    m_session->engine()->updateNote(noteId, p);
    if (m_pane->noteId() == noteId) m_pane->syncBodyFromEngine();
}

void MainWindow::openDiff(const QString &baseId, const QString &otherId) {
    if (!m_session || baseId.isEmpty()) return;
    m_pane->flush();
    NoteDiffDialog dlg(m_session, baseId, otherId, this);
    connect(&dlg, &NoteDiffDialog::openNoteRequested, this, [this](const QString &id) { openNote(id); });
    dlg.exec();
    m_pane->syncBodyFromEngine();
}

// --- sidebar ---

void MainWindow::showPage(Page page, bool focus) {
    m_page = page;
    m_side->setCurrentIndex(int(page));
    QSettings().setValue(QStringLiteral("window/page"), int(page));
    if (m_shelfCollapsed) toggleShelf();
    else syncRail();
    if (focus && page == NotesPage) m_shelf->focusTree();
}

void MainWindow::syncRail() {
    for (int i = 0; i < m_railPages.size(); ++i)
        m_railPages[i]->setChecked(!m_shelfCollapsed && m_page == Page(i));
}

void MainWindow::refreshRailIcons() {
    const Tokens &k = Theme::t();
    const qreal dpr = devicePixelRatioF();
    const auto withBadge = [&](QPixmap base, int size) -> QPixmap {
        if (m_agendaBadgeCount <= 0 || size < 18) return base;
        QPainter p(&base);
        p.setRenderHint(QPainter::Antialiasing);
        const QString text = m_agendaBadgeCount > 99 ? QStringLiteral("99+") : QString::number(m_agendaBadgeCount);
        const qreal h = std::max<qreal>(10.0, size * 0.48);
        QFont f = Theme::uiFont(std::max<qreal>(7.5, size * 0.34), QFont::Bold);
        p.setFont(f);
        const qreal tw = QFontMetricsF(f).horizontalAdvance(text);
        const qreal w = std::max(h, tw + 5.0);
        const QRectF pill(size - w, 0.0, w, h);
        p.setPen(Qt::NoPen);
        p.setBrush(m_agendaBadgeOverdue ? k.danger : k.ivy);
        p.drawRoundedRect(pill, h / 2.0, h / 2.0);
        p.setPen(Qt::white);
        p.drawText(pill, Qt::AlignCenter, text);
        p.end();
        return base;
    };

    QList<QAction *> all = m_railPages;
    if (m_railSettings) all << m_railSettings;
    for (QAction *a : all) {
        const QString name = a->data().toString();
        const bool isAgenda = (m_railPages.size() > AgendaPage && a == m_railPages[AgendaPage]);
        QIcon icon;
        for (int size : {16, 20, 24, 32}) {
            auto pm = [&](const QColor &c) {
                QPixmap p = icons::pixmap(name, c, size, dpr);
                return isAgenda ? withBadge(p, size) : p;
            };
            icon.addPixmap(pm(k.muted), QIcon::Normal, QIcon::Off);
            icon.addPixmap(pm(k.ink), QIcon::Active, QIcon::Off);
            icon.addPixmap(pm(k.ivy), QIcon::Normal, QIcon::On);
            icon.addPixmap(pm(k.ivy), QIcon::Active, QIcon::On);
        }
        a->setIcon(icon);
        if (isAgenda) {
            QString tip = T("日程") + QStringLiteral(" (Ctrl+2)");
            if (m_agendaBadgeCount > 0)
                tip += QStringLiteral(" · ") + QString::number(m_agendaBadgeCount);
            a->setToolTip(tip);
        }
    }
}

void MainWindow::dropEmptyDraft(const QString &next) {
    if (m_draftId.isEmpty() || m_draftId == next || !m_session) return;
    const QString id = m_draftId;
    const QString initialTitle = m_draftTitle;
    m_draftId.clear();
    m_draftTitle.clear();
    if (m_pane->noteId() == id) m_pane->flush();
    const Note *n = m_session->engine()->note(id);
    if (n && !n->trashed() && n->title.trimmed() == initialTitle.trimmed() && n->body.trimmed().isEmpty() && n->tags.isEmpty())
        m_session->engine()->deleteNote(id);
}

void MainWindow::newNote(const QString &folderId) {
    if (!m_session) return;
    m_pane->flush();
    NoteInit init;
    init.folderId = folderId;
    if (folderId.isEmpty()) {
        const QString defFolder = Prefs::instance()->defaultNoteFolder();
        if (defFolder == QLatin1String("__inbox__")) {
            init.folderId.clear();
        } else if (!defFolder.isEmpty()) {
            const auto &folders = m_session->engine()->folders();
            const auto it = folders.constFind(defFolder);
            if (it != folders.constEnd() && !it->deleted) {
                init.folderId = defFolder;
            } else if (const Note *cur = m_session->engine()->note(m_pane->noteId()); cur && !cur->trashed()) {
                init.folderId = cur->folderId;
            }
        } else {
            // New notes go next to the open one.
            if (const Note *cur = m_session->engine()->note(m_pane->noteId()); cur && !cur->trashed()) init.folderId = cur->folderId;
        }
    }
    const QString titleTpl = Prefs::instance()->defaultNoteTitle();
    if (!titleTpl.trimmed().isEmpty())
        init.title = fmt::formatNoteTitle(titleTpl, QDateTime::currentDateTime(), i18n::isZh());
    const Note n = m_session->engine()->createNote(init);
    openNoteInMode(n.id, true);
    m_draftId = n.id;
    m_draftTitle = init.title;
    if (init.title.trimmed().isEmpty()) m_pane->focusTitle();
    else m_pane->focusEditor();
}

void MainWindow::moveNote(const QString &id) { moveNotes({id}); }

void MainWindow::moveNotes(const QStringList &ids) {
    if (!m_session || ids.isEmpty()) return;
    const Note *first = m_session->engine()->note(ids.first());
    if (!first) return;
    QString target;
    const QString title = ids.size() == 1 ? T("移动到")
                                          : (i18n::isZh() ? QStringLiteral("把 %1 篇笔记移动到") : QStringLiteral("Move %1 notes to")).arg(ids.size());
    if (!dialogs::pickFolder(m_session->engine(), this, title, first->folderId, &target)) return;
    m_pane->flush();
    for (const QString &id : ids) {
        const Note *n = m_session->engine()->note(id);
        if (!n || n->trashed() || n->folderId == target) continue;
        NotePatch p;
        p.folderId = target;
        m_session->engine()->updateNote(id, p);
    }
    if (!m_pane->noteId().isEmpty()) m_shelf->setCurrentNote(m_pane->noteId());
}

void MainWindow::duplicateNote(const QString &id) {
    if (!m_session) return;
    m_pane->flush();
    const Note *n = m_session->engine()->note(id);
    if (!n) return;
    NoteInit init;
    init.title = (n->title.isEmpty() ? T("无标题") : n->title) + T(" 副本");
    init.body = n->body;
    init.folderId = n->folderId;
    init.tags = n->tags;
    openNote(m_session->engine()->createNote(init).id);
}

void MainWindow::emptyTrash() {
    if (!m_session) return;
    QStringList ids;
    for (const auto &n : m_session->engine()->notes())
        if (n.trashed()) ids << n.id;
    if (ids.isEmpty()) return;
    const QString text = (i18n::isZh() ? QStringLiteral("永久删除回收站中的 %1 篇笔记？此操作无法撤销。") : QStringLiteral("Permanently delete %1 notes in the trash? This cannot be undone.")).arg(ids.size());
    if (!dialogs::confirm(this, T("清空回收站"), text, T("清空"))) return;
    if (ids.contains(m_pane->noteId())) openNote(QString());
    for (const QString &id : ids) m_session->engine()->deleteNote(id);
}

void MainWindow::showSearch(const QString &initial) {
    if (!m_session) return;
    m_pane->flush();
    QList<PaletteCommand> cmds{
        {T("新建笔记"), QStringLiteral("Ctrl+N"), QStringLiteral("edit"), [this] { newNote(QString()); }},
        {T("新建笔记本"), QString(), QStringLiteral("folder"), [this] {
             bool ok = false;
             const QString name = QInputDialog::getText(this, T("新建笔记本"), T("名称"), QLineEdit::Normal, QString(), &ok).trimmed();
             if (ok && !name.isEmpty() && m_session) m_session->engine()->createFolder(name.left(100));
         }},
        {T("收起或展开书架"), QStringLiteral("Ctrl+\\"), QStringLiteral("sidebar"), [this] { toggleShelf(); }},
        {T("日程"), QStringLiteral("Ctrl+2"), QStringLiteral("calendar"), [this] { showPage(AgendaPage, false); }},
        {T("媒体库"), QStringLiteral("Ctrl+3"), QStringLiteral("image"), [this] { showPage(MediaPage, false); }},
        {T("所见即所得"), QString(), QStringLiteral("eye"), [this] { m_pane->setEditorMode(EditorMode::Live); }},
        {T("源码"), QString(), QStringLiteral("code"), [this] { m_pane->setEditorMode(EditorMode::Source); }},
        {T("双栏"), QString(), QStringLiteral("split"), [this] { m_pane->setEditorMode(EditorMode::Split); }},
        {T("阅读模式"), QStringLiteral("Ctrl+Shift+E"), QStringLiteral("book"), [this] {
             if (!m_pane->noteId().isEmpty()) m_pane->toggleReading();
         }},
        {T("浅色主题"), QString(), QStringLiteral("sun"), [] { Prefs::instance()->setTheme(1); }},
        {T("深色主题"), QString(), QStringLiteral("moon"), [] { Prefs::instance()->setTheme(2); }},
        {T("跟随系统主题"), QString(), QStringLiteral("monitor"), [] { Prefs::instance()->setTheme(0); }},
        {T("导入 Markdown…"), QString(), QStringLiteral("upload"), [this] { importMarkdown(); }},
        {T("导出为 Markdown…"), QString(), QStringLiteral("download"), [this] { exportMarkdown(); }},
        {T("新建或接入笔记库…"), QString(), QStringLiteral("vault"), [this] { addVault(); }},
        {T("设置"), QStringLiteral("Ctrl+,"), QStringLiteral("settings"), [this] { showSettings(); }},
        {T("清空回收站"), QString(), QStringLiteral("trash"), [this] { emptyTrash(); }},
    };
    if (!m_pane->noteId().isEmpty()) {
        cmds.insert(9, {T("对比差异 (Diff)"), QString(), QStringLiteral("diff"), [this] { openDiff(m_pane->noteId()); }});
        cmds.insert(10, {T("设置日期与重复"), QStringLiteral("Ctrl+Shift+D"), QStringLiteral("calendar"), [this] {
            if (m_pane->isReading()) m_pane->setReading(false);
            m_pane->focusEditor();
            m_pane->editor()->openDuePicker();
        }});
    }
    if (m_session->isRemote()) cmds.append({T("立即同步"), QStringLiteral("Ctrl+R"), QStringLiteral("refresh"), [this] { if (m_session) m_session->wake(); }});
    for (const VaultInfo &v : vaults::list())
        if (v.id != m_session->info().id)
            cmds.append({T("切换到") + QStringLiteral(" ") + v.name, QString(), QStringLiteral("vault"), [this, id = v.id] { openVault(id); }});
    auto *palette = new CommandPalette(m_session->engine(), cmds, this);
    connect(palette, &CommandPalette::openNote, this, [this](const QString &id) {
        openNote(id);
        m_pane->focusEditor();
    });
    palette->open(initial);
}

void MainWindow::showSettings(SettingsPage page) {
    SettingsDialog dlg(m_session, this);
    if (page == VaultPage) dlg.showVaultTab();
    else if (page == AboutPage) dlg.showAboutTab();
    connect(&dlg, &SettingsDialog::importMarkdown, this, &MainWindow::importMarkdown, Qt::QueuedConnection);
    connect(&dlg, &SettingsDialog::importVault, this, &MainWindow::importVault, Qt::QueuedConnection);
    connect(&dlg, &SettingsDialog::exportMarkdown, this, &MainWindow::exportMarkdown, Qt::QueuedConnection);
    connect(&dlg, &SettingsDialog::checkUpdates, this, [this, dlgPtr = QPointer<SettingsDialog>(&dlg)] {
        updater::check(this, true, [dlgPtr](updater::Outcome, const QString &message) {
            if (dlgPtr) dlgPtr->setUpdateStatus(false, message);
        });
    });
    connect(&dlg, &SettingsDialog::removeVault, this, [this, &dlg] {
        dlg.accept();
        QTimer::singleShot(0, this, &MainWindow::removeVault);
    });
    connect(&dlg, &SettingsDialog::vaultRenamed, this, [this] {
        m_shelf->setSession(m_session);
        m_shelf->setCurrentNote(m_pane->noteId());
        rebuildVaultMenu();
        updateTitle();
    });
    dlg.exec();
}

void MainWindow::toggleShelf() {
    m_shelfCollapsed = !m_shelfCollapsed;
    QSettings().setValue(QStringLiteral("window/shelfCollapsed"), m_shelfCollapsed);
    syncRail();
    if (Theme::reducedMotion()) {
        m_side->setVisible(!m_shelfCollapsed);
        return;
    }
    const int width = std::max(220, QSettings().value(QStringLiteral("window/shelf"), 296).toInt());
    if (!m_shelfCollapsed) {
        m_side->setMaximumWidth(0);
        m_side->show();
    }
    auto *a = new QPropertyAnimation(m_side, "maximumWidth", this);
    a->setDuration(220);
    a->setEasingCurve(Theme::easeOut());
    a->setStartValue(m_shelfCollapsed ? m_side->width() : 0);
    a->setEndValue(m_shelfCollapsed ? 0 : width);
    connect(a, &QPropertyAnimation::finished, this, [this] {
        m_side->setMaximumWidth(QWIDGETSIZE_MAX);
        m_side->setVisible(!m_shelfCollapsed);
        if (!m_shelfCollapsed) m_split->setSizes({QSettings().value(QStringLiteral("window/shelf"), 296).toInt(), m_split->width()});
    });
    if (m_shelfCollapsed) {
        QSettings().setValue(QStringLiteral("window/shelf"), m_side->width());
        m_side->setMinimumWidth(0);
    }
    a->start(QAbstractAnimation::DeleteWhenStopped);
    if (!m_shelfCollapsed) QTimer::singleShot(240, this, [this] { m_side->setMinimumWidth(220); });
}

// --- import and export ---

void MainWindow::runJob(TransferJob *job, const QString &title) {
    auto *progress = new QProgressDialog(title, T("取消"), 0, 0, this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(300);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    connect(progress, &QProgressDialog::canceled, job, &TransferJob::cancel);
    connect(job, &TransferJob::progress, progress, [progress](int done, int total, const QString &what) {
        progress->setMaximum(total);
        progress->setValue(done);
        progress->setLabelText(what);
    });
    connect(job, &TransferJob::finished, this, [this, job, progress](bool ok, const QString &summary) {
        progress->close();
        progress->deleteLater();
        job->deleteLater();
        Toaster::show(summary, !ok);
        m_shelf->rebuild();
    });
    job->start();
}

void MainWindow::importMarkdown() {
    if (!m_session) return;
    QMessageBox box(QMessageBox::Question, T("导入 Markdown"), T("导入单个文件，还是整个文件夹（子文件夹会成为笔记本）？"), QMessageBox::NoButton, this);
    QPushButton *files = box.addButton(T("选择文件…"), QMessageBox::AcceptRole);
    QPushButton *folder = box.addButton(T("选择文件夹…"), QMessageBox::AcceptRole);
    box.addButton(T("取消"), QMessageBox::RejectRole);
    box.exec();
    QStringList paths;
    if (box.clickedButton() == files) {
        paths = QFileDialog::getOpenFileNames(this, T("导入 Markdown"), QString(), QStringLiteral("Markdown (*.md *.markdown *.txt)"));
    } else if (box.clickedButton() == folder) {
        const QString dir = QFileDialog::getExistingDirectory(this, T("导入 Markdown"));
        if (!dir.isEmpty()) paths << dir;
    }
    if (paths.isEmpty()) return;
    QString target;
    if (!m_session->engine()->folders().isEmpty() &&
        !dialogs::pickFolder(m_session->engine(), this, T("导入到"), QString(), &target))
        return;
    runJob(transfer::importMarkdown(m_session, paths, target, this), T("正在导入…"));
}

void MainWindow::importVault() {
    if (!m_session) return;
    QStringList names;
    QList<VaultInfo> others;
    for (const VaultInfo &v : vaults::list()) {
        if (v.id == m_session->info().id) continue;
        others << v;
        names << v.name + (v.isRemote() ? QString() : QStringLiteral(" (") + T("本地") + u')');
    }
    if (others.isEmpty()) {
        Toaster::show(T("这台电脑上没有其他笔记库"));
        return;
    }
    bool ok = false;
    const QString picked = QInputDialog::getItem(this, T("从其他笔记库导入"),
                                                 T("把哪个笔记库的笔记导入到「%1」？\n笔记本、标签、附件都会一起复制，回收站里的笔记除外。").arg(m_session->info().name),
                                                 names, 0, false, &ok);
    if (!ok) return;
    const VaultInfo source = others.value(names.indexOf(picked));
    runJob(transfer::importVault(m_session, source, this), T("正在导入…"));
}

void MainWindow::exportMarkdown() {
    if (!m_session) return;
    const QString parent = QFileDialog::getExistingDirectory(this, T("导出到文件夹"));
    if (parent.isEmpty()) return;
    QString name = m_session->info().name;
    name.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|])")), QStringLiteral("_"));
    QString dir = parent + u'/' + name + QStringLiteral(" ") + QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd"));
    for (int i = 2; QFileInfo::exists(dir); ++i) dir = parent + u'/' + name + QStringLiteral(" ") + QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd")) + QStringLiteral(" (%1)").arg(i);
    m_pane->flush();
    runJob(transfer::exportMarkdown(m_session, dir, this), T("正在导出…"));
}

void MainWindow::removeVault() {
    if (!m_session) return;
    const VaultInfo info = m_session->info();
    const SyncState st = m_session->engine()->state();
    QString text;
    if (info.isRemote()) {
        text = T("退出登录并从这台电脑移除「%1」？服务器上的笔记不受影响。").arg(info.name);
        if (st.blocked > 0 || st.pending + st.uploads > 0)
            text += u'\n' + (i18n::isZh() ? QStringLiteral("\n注意：还有 %1 条更改没有同步到服务器（其中 %2 条被拒绝），移除后会丢失。")
                                          : QStringLiteral("\nNote: %1 changes have not reached the server (%2 refused) and will be lost."))
                                .arg(st.pending + st.uploads + st.blocked).arg(st.blocked);
    } else {
        text = T("永久删除本地笔记库「%1」及其中全部笔记和附件？此操作无法撤销。").arg(info.name);
    }
    if (!dialogs::confirm(this, info.isRemote() ? T("退出登录") : T("删除笔记库"), text, info.isRemote() ? T("退出并移除") : T("删除"))) return;
    if (!info.isRemote()) {
        bool ok = false;
        const QString typed = QInputDialog::getText(this, T("删除笔记库"), T("请输入笔记库名称以确认："), QLineEdit::Normal, QString(), &ok);
        if (!ok || typed.trimmed() != info.name) return;
    }
    m_pane->flush();
    QPointer<MainWindow> self(this);
    setEnabled(false);
    Toaster::show(info.isRemote() ? T("正在同步并退出…") : T("正在删除…"));
    m_session->signOut([self, info] {
        if (!self) return;
        self->setEnabled(true);
        self->closeVault();
        vaults::remove(info.id);
        Toaster::show(info.isRemote() ? T("已退出并移除") : T("已删除"));
        const auto rest = vaults::list();
        if (!rest.isEmpty() && self->openVault(rest.first().id)) return;
        self->rebuildVaultMenu();
        self->m_stack->setCurrentWidget(self->m_empty);
        self->addVault();
    });
}

// --- window ---

void MainWindow::closeEvent(QCloseEvent *e) {
    QSettings s;
    s.setValue(QStringLiteral("window/geometry"), saveGeometry());
    if (!m_shelfCollapsed) s.setValue(QStringLiteral("window/shelf"), m_side->width());
    if (m_session) {
        m_pane->flush();
        m_session->engine()->persistNow();
        // Give a pending push one short chance before quitting.
        if (m_session->isRemote() && m_session->signedIn() && m_session->engine()->state().pending > 0 && !m_session->engine()->running()) {
            QEventLoop loop;
            connect(m_session->engine(), &SyncEngine::roundFinished, &loop, &QEventLoop::quit);
            QTimer::singleShot(3000, &loop, &QEventLoop::quit);
            m_session->engine()->sync();
            loop.exec(QEventLoop::ExcludeUserInputEvents);
        }
    }
    closeVault();
    e->accept();
}

void MainWindow::changeEvent(QEvent *e) {
    QMainWindow::changeEvent(e);
    if (e->type() == QEvent::ActivationChange && isActiveWindow() && m_session) m_session->wake();
}

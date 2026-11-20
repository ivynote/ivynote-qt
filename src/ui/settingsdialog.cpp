#include "ui/settingsdialog.h"

#include "core/format.h"
#include "core/i18n.h"
#include "storage/crypto.h"
#include "storage/secrets.h"
#include "sync/api.h"
#include "sync/engine.h"
#include "ui/dialogs.h"
#include "ui/theme.h"
#include "ui/toast.h"
#include "ui/vaultdialog.h"
#include "ui/widgets.h"
#include "vault/backup.h"
#include "vault/files.h"
#include "vault/session.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>

// --- prefs ---

Prefs *Prefs::instance() {
    static Prefs *p = new Prefs(nullptr);
    return p;
}

int Prefs::theme() const { return QSettings().value(QStringLiteral("ui/theme"), 0).toInt(); }
int Prefs::lang() const { return QSettings().value(QStringLiteral("ui/lang"), int(i18n::systemLang())).toInt(); }
int Prefs::fontSize() const { return std::clamp(QSettings().value(QStringLiteral("ui/fontSize"), 16).toInt(), 13, 22); }
int Prefs::editorMode() const { return std::clamp(QSettings().value(QStringLiteral("ui/editorMode"), 0).toInt(), 0, 3); }
bool Prefs::reduceMotion() const { return QSettings().value(QStringLiteral("ui/reduceMotion"), false).toBool(); }
QString Prefs::defaultNoteFolder() const { return QSettings().value(QStringLiteral("ui/defaultNoteFolder"), QString()).toString(); }
QString Prefs::defaultNoteTitle() const { return QSettings().value(QStringLiteral("ui/defaultNoteTitle"), QString()).toString(); }

void Prefs::setTheme(int v) { QSettings().setValue(QStringLiteral("ui/theme"), v); emit changed(); }
void Prefs::setLang(int v) { QSettings().setValue(QStringLiteral("ui/lang"), v); emit changed(); }
void Prefs::setFontSize(int v) { QSettings().setValue(QStringLiteral("ui/fontSize"), std::clamp(v, 13, 22)); emit changed(); }
void Prefs::setEditorMode(int v) { QSettings().setValue(QStringLiteral("ui/editorMode"), std::clamp(v, 0, 3)); emit changed(); }
void Prefs::setReduceMotion(bool v) { QSettings().setValue(QStringLiteral("ui/reduceMotion"), v); emit changed(); }
void Prefs::setDefaultNoteFolder(const QString &v) { QSettings().setValue(QStringLiteral("ui/defaultNoteFolder"), v); emit changed(); }
void Prefs::setDefaultNoteTitle(const QString &v) { QSettings().setValue(QStringLiteral("ui/defaultNoteTitle"), v); emit changed(); }

namespace {

// Word-wrapped QLabel (especially with rich-text links or slash-separated paths)
// computes sizeHint()/minimumSizeHint() at a narrow fallback width, inflating
// row height even when the label fits in fewer lines. Size from heightForWidth()
// at the current width (defaulting to one line before layout) instead.
class WrapLabel : public QLabel {
public:
    using QLabel::QLabel;
    QSize minimumSizeHint() const override {
        const QMargins m = contentsMargins();
        return {0, fontMetrics().height() + m.top() + m.bottom()};
    }
    QSize sizeHint() const override {
        const QMargins m = contentsMargins();
        if (wordWrap() && width() > 0) {
            const int h = heightForWidth(width());
            if (h > 0) return {width(), h};
        }
        return {QLabel::sizeHint().width(), fontMetrics().height() + m.top() + m.bottom()};
    }
protected:
    void resizeEvent(QResizeEvent *e) override {
        QLabel::resizeEvent(e);
        if (e->size().width() != e->oldSize().width()) updateGeometry();
    }
};

QLabel *note(const QString &text, QWidget *parent) {
    auto *l = mutedLabel(text, parent, 12);
    l->setWordWrap(true);
    return l;
}

const char *kModes[] = {"live", "source", "split", "read"};

// Cross-device preferences, aligned with web/Tauri /me/prefs schema.
void pushPrefs(Session *s) {
    if (!s || !s->isRemote() || !s->signedIn()) return;
    Prefs *p = Prefs::instance();
    s->api()->putJson(QStringLiteral("/me/prefs"),
                      QJsonObject{{"editorMode", QLatin1String(kModes[p->editorMode()])},
                                  {"fontSize", p->fontSize()},
                                  {"defaultNoteFolder", p->defaultNoteFolder()},
                                  {"defaultNoteTitle", p->defaultNoteTitle()}},
                      s, [](const ApiReply &) {});
}

void collectFolderOptions(const QHash<QString, Folder> &folders, const QString &parentId, int depth, QComboBox *box) {
    QList<Folder> children;
    for (const Folder &f : folders) {
        if (!f.deleted && f.parentId == parentId) children << f;
    }
    std::sort(children.begin(), children.end(), [](const Folder &a, const Folder &b) {
        const int cmp = a.name.compare(b.name, Qt::CaseInsensitive);
        return cmp != 0 ? cmp < 0 : a.id < b.id;
    });
    const QString indent(depth, QChar(0x3000));
    for (const Folder &f : children) {
        box->addItem(indent + f.name, f.id);
        collectFolderOptions(folders, f.id, depth + 1, box);
    }
}

} // namespace

SettingsDialog::SettingsDialog(Session *session, QWidget *parent) : QDialog(parent), m_session(session) {
    setWindowTitle(T("设置"));
    setMinimumSize(580, 580);
    auto *col = new QVBoxLayout(this);
    m_tabs = new QTabWidget(this);
    m_tabs->addTab(generalTab(), T("通用"));
    if (session) m_tabs->addTab(m_vault = vaultTab(), T("笔记库"));
    if (session && session->isRemote()) m_tabs->addTab(accountTab(), T("账户"));
    m_tabs->addTab(m_about = aboutTab(), T("关于"));
    col->addWidget(m_tabs);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setText(T("关闭"));
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    col->addWidget(buttons);
}

void SettingsDialog::showVaultTab() {
    if (m_vault) m_tabs->setCurrentWidget(m_vault);
}

void SettingsDialog::showAboutTab() { m_tabs->setCurrentWidget(m_about); }

QWidget *SettingsDialog::generalTab() {
    auto *w = new QWidget;
    auto *outer = new QVBoxLayout(w);
    outer->setContentsMargins(20, 18, 20, 18);
    outer->setSpacing(14);
    auto *form = new QFormLayout;
    form->setVerticalSpacing(12);
    Prefs *p = Prefs::instance();

    auto *theme = new QComboBox(w);
    theme->addItems({T("跟随系统"), T("浅色"), T("深色")});
    theme->setCurrentIndex(p->theme());
    connect(theme, &QComboBox::currentIndexChanged, p, &Prefs::setTheme);
    form->addRow(T("主题"), theme);

    auto *lang = new QComboBox(w);
    lang->addItems({QStringLiteral("中文"), QStringLiteral("English")});
    lang->setCurrentIndex(p->lang());
    auto *langNote = note(T("语言在重新打开窗口后完全生效。"), w);
    langNote->hide();
    connect(lang, &QComboBox::currentIndexChanged, this, [p, langNote](int i) {
        p->setLang(i);
        langNote->show();
    });
    form->addRow(T("语言"), lang);
    form->addRow(QString(), langNote);

    auto *size = new QSpinBox(w);
    size->setRange(13, 22);
    size->setSuffix(QStringLiteral(" px"));
    size->setValue(p->fontSize());
    connect(size, &QSpinBox::valueChanged, this, [this, p](int v) {
        p->setFontSize(v);
        pushPrefs(m_session);
    });
    form->addRow(T("正文字号"), size);

    auto *mode = new QComboBox(w);
    mode->setObjectName(QStringLiteral("defaultOpenModeBox"));
    mode->addItems({T("所见即所得"), T("源码"), T("双栏"), T("阅读模式")});
    mode->setCurrentIndex(p->editorMode());
    connect(mode, &QComboBox::currentIndexChanged, this, [this, p](int v) {
        p->setEditorMode(v);
        pushPrefs(m_session);
    });
    form->addRow(T("默认打开模式"), mode);

    auto *motion = new QCheckBox(T("减少动态效果"), w);
    motion->setChecked(p->reduceMotion());
    connect(motion, &QCheckBox::toggled, p, &Prefs::setReduceMotion);
    form->addRow(QString(), motion);
    outer->addLayout(form);

    // Default New Note folder & title template.
    auto *tplGroup = new QGroupBox(T("新建笔记默认设置"), w);
    auto *tplForm = new QFormLayout(tplGroup);
    tplForm->setVerticalSpacing(10);

    auto *folderBox = new QComboBox(tplGroup);
    folderBox->setObjectName(QStringLiteral("defaultNoteFolderBox"));
    folderBox->addItem(T("跟随当前位置"), QString());
    folderBox->addItem(T("未归档"), QStringLiteral("__inbox__"));
    if (m_session && m_session->engine()) {
        collectFolderOptions(m_session->engine()->folders(), QString(), 0, folderBox);
    }
    const int folderIdx = folderBox->findData(p->defaultNoteFolder());
    folderBox->setCurrentIndex(folderIdx >= 0 ? folderIdx : 0);
    connect(folderBox, &QComboBox::currentIndexChanged, this, [this, p, folderBox](int idx) {
        p->setDefaultNoteFolder(folderBox->itemData(idx).toString());
        pushPrefs(m_session);
    });
    tplForm->addRow(T("默认文件夹"), folderBox);

    auto *tplEdit = new QLineEdit(p->defaultNoteTitle(), tplGroup);
    tplEdit->setObjectName(QStringLiteral("defaultNoteTitleEdit"));
    tplEdit->setFont(Theme::mono(13));
    tplEdit->setMaxLength(200);
    tplEdit->setPlaceholderText(T("留空则新建空白标题，支持 YYYY-MM-DD HH:mm 等"));
    tplForm->addRow(T("默认标题模板"), tplEdit);

    auto *presetsRow = new QHBoxLayout;
    presetsRow->setSpacing(6);
    const QList<QPair<QString, QString>> presets{
        {QStringLiteral("YYYY-MM-DD"), QStringLiteral("YYYY-MM-DD")},
        {QStringLiteral("YYYY-MM-DD HH:mm"), QStringLiteral("YYYY-MM-DD HH:mm")},
        {QStringLiteral("YYYY-MM-DD-随记"), QStringLiteral("YYYY-MM-DD-随记")},
        {QStringLiteral("YYYY年MM月DD日 ddd"), QStringLiteral("YYYY年MM月DD日 ddd")},
        {T("清空"), QString()},
    };
    for (const auto &[label, val] : presets) {
        auto *btn = new QPushButton(label, tplGroup);
        btn->setFlat(false);
        btn->setFont(Theme::uiFont(11.5));
        connect(btn, &QPushButton::clicked, tplEdit, [tplEdit, val] { tplEdit->setText(val); });
        presetsRow->addWidget(btn);
    }
    presetsRow->addStretch(1);
    tplForm->addRow(QString(), presetsRow);

    auto *preview = mutedLabel(QString(), tplGroup, 12.5);
    preview->setObjectName(QStringLiteral("defaultNoteTitlePreview"));
    auto updatePreview = [preview](const QString &tpl) {
        const QString formatted = fmt::formatNoteTitle(tpl, QDateTime::currentDateTime(), i18n::isZh());
        preview->setText(formatted.isEmpty() ? T("预览：无标题（空白）") : T("预览：") + formatted);
    };
    updatePreview(tplEdit->text());
    tplForm->addRow(QString(), preview);
    tplForm->addRow(QString(), note(T("可用占位符：YYYY（年）、YY（短年）、MM（月）、DD（日）、HH（时）、mm（分）、ss（秒）、ddd（星期）。"), tplGroup));

    auto *pushTimer = new QTimer(this);
    pushTimer->setSingleShot(true);
    pushTimer->setInterval(400);
    connect(pushTimer, &QTimer::timeout, this, [this] { pushPrefs(m_session); });
    connect(tplEdit, &QLineEdit::textChanged, this, [p, updatePreview, pushTimer](const QString &text) {
        p->setDefaultNoteTitle(text);
        updatePreview(text);
        pushTimer->start();
    });
    outer->addWidget(tplGroup);

    if (m_session && m_session->isRemote())
        outer->addWidget(note(T("字号、编辑模式与新建笔记模板会同步到这个账户的其他设备。"), w));
    outer->addStretch(1);
    return w;
}

QWidget *SettingsDialog::vaultTab() {
    auto *scroll = new QScrollArea;
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    auto *w = new QWidget;
    auto *col = new QVBoxLayout(w);
    col->setContentsMargins(20, 20, 20, 20);
    col->setSpacing(14);
    const VaultInfo &info = m_session->info();

    auto *formBox = new QWidget(w);
    auto *grid = new QGridLayout(formBox);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(10);
    grid->setColumnStretch(1, 1);

    int fRow = 0;
    auto addFieldRow = [&](const QString &labelText, QWidget *field, Qt::Alignment labelAlign = Qt::AlignRight | Qt::AlignVCenter,
                           const QString &labelObjName = QString()) {
        if (!labelText.isEmpty()) {
            auto *lbl = new QLabel(labelText, formBox);
            if (!labelObjName.isEmpty()) lbl->setObjectName(labelObjName);
            grid->addWidget(lbl, fRow, 0, labelAlign);
        }
        grid->addWidget(field, fRow++, 1);
    };

    auto *name = new QLineEdit(info.name, formBox);
    connect(name, &QLineEdit::editingFinished, this, [this, name] {
        if (!m_session || name->text().trimmed().isEmpty() || name->text().trimmed() == m_session->info().name) return;
        m_session->rename(name->text());
        emit vaultRenamed();
    });
    addFieldRow(T("名称"), name);

    auto *typeLbl = new QLabel(info.isRemote() ? T("同步笔记库") : T("本地笔记库（不同步）"), formBox);
    typeLbl->setObjectName(QStringLiteral("vaultTypeLabel"));
    addFieldRow(T("类型"), typeLbl);

    if (info.isRemote()) {
        auto *server = new WrapLabel(QStringLiteral("<a href=\"%1\">%1</a>").arg(info.server.toHtmlEscaped()), formBox);
        server->setOpenExternalLinks(true);
        server->setWordWrap(true);
        server->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        server->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        addFieldRow(T("服务器"), server, Qt::AlignRight | Qt::AlignTop);
        addFieldRow(T("账户"), new QLabel(info.username, formBox));
    }

    auto *dir = new WrapLabel(QStringLiteral("<a href=\"%1\">%2</a>").arg(QUrl::fromLocalFile(info.dir()).toString(), QDir::toNativeSeparators(info.dir()).toHtmlEscaped()), formBox);
    dir->setObjectName(QStringLiteral("vaultLocationLabel"));
    dir->setOpenExternalLinks(true);
    dir->setWordWrap(true);
    dir->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    dir->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    addFieldRow(T("位置"), dir, Qt::AlignRight | Qt::AlignTop, QStringLiteral("vaultLocationTitle"));

    auto *crypt = new QWidget(formBox);
    auto *cryptRow = new QHBoxLayout(crypt);
    cryptRow->setContentsMargins(0, 0, 0, 0);
    cryptRow->addWidget(new QLabel(info.encrypted ? T("已加密（XChaCha20-Poly1305）") : T("未加密"), crypt));
    if (info.encrypted) {
        auto *show = new QPushButton(T("显示恢复密钥"), crypt);
        connect(show, &QPushButton::clicked, this, [this] {
            if (!m_session) return;
            dialogs::showRecoveryKey(this, m_session->info().name, VaultCipher::encodeRecoveryKey(m_session->vaultKey()), false);
        });
        cryptRow->addWidget(show);
    }
    cryptRow->addStretch(1);
    addFieldRow(T("本机加密"), crypt);

    if (secrets::insecure())
        addFieldRow(QString(), note(T("⚠ 系统钥匙串不可用，令牌和密钥保存在仅当前用户可读的文件中。"), formBox));

    if (info.isRemote()) {
        auto *cache = new QWidget(formBox);
        auto *cacheRow = new QHBoxLayout(cache);
        cacheRow->setContentsMargins(0, 0, 0, 0);
        auto *size = new QLabel(fmt::bytes(m_session->files()->cacheBytes()), cache);
        auto *clear = new QPushButton(T("清除"), cache);
        connect(clear, &QPushButton::clicked, this, [this, size] {
            if (!m_session) return;
            m_session->files()->clearCache();
            size->setText(fmt::bytes(m_session->files()->cacheBytes()));
        });
        cacheRow->addWidget(size);
        cacheRow->addWidget(clear);
        cacheRow->addStretch(1);
        addFieldRow(T("附件缓存"), cache);
    }
    col->addWidget(formBox);

    {
        auto *bak = new QGroupBox(T("本地自动备份"), w);
        bak->setObjectName(QStringLiteral("localBackupGroup"));
        auto *bakCol = new QVBoxLayout(bak);
        bakCol->setSpacing(8);
        const backup::Config cfg = backup::loadConfig(info);
        const backup::ManifestSummary sum = backup::readManifestSummary(cfg.directory);
        const backup::SpaceCheck space = backup::checkSpace(cfg.directory, backup::estimateVaultBytes(info));

        auto *bakGrid = new QGridLayout;
        bakGrid->setContentsMargins(0, 0, 0, 0);
        bakGrid->setHorizontalSpacing(12);
        bakGrid->setVerticalSpacing(6);
        bakGrid->setColumnStretch(1, 1);

        int bRow = 0;
        QString statusText = cfg.enabled
            ? (i18n::isZh() ? QStringLiteral("已开启（每 %1 分钟增量备份）").arg(cfg.intervalMinutes)
                            : QStringLiteral("Enabled (incremental every %1 min)").arg(cfg.intervalMinutes))
            : T("已关闭");
        if (sum.exists && sum.lastBackupAt > 0) {
            const QString when = QDateTime::fromMSecsSinceEpoch(sum.lastBackupAt).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
            statusText += QStringLiteral(" · ") + T("上次备份：") + when;
        }
        auto *statusLbl = new WrapLabel(statusText, bak);
        statusLbl->setObjectName(QStringLiteral("localBackupStatusLabel"));
        statusLbl->setWordWrap(true);
        statusLbl->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        statusLbl->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        bakGrid->addWidget(new QLabel(T("状态"), bak), bRow, 0, Qt::AlignLeft | Qt::AlignTop);
        bakGrid->addWidget(statusLbl, bRow++, 1);

        auto *bakDir = new WrapLabel(cfg.directory.isEmpty() ? T("未设置") : QDir::toNativeSeparators(cfg.directory), bak);
        bakDir->setWordWrap(true);
        bakDir->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        bakDir->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        bakDir->setTextInteractionFlags(Qt::TextSelectableByMouse);
        bakGrid->addWidget(new QLabel(T("备份目录"), bak), bRow, 0, Qt::AlignLeft | Qt::AlignTop);
        bakGrid->addWidget(bakDir, bRow++, 1);

        if (space.valid) {
            const QString spText = (i18n::isZh() ? QStringLiteral("可用 %1 / 总计 %2") : QStringLiteral("%1 free / %2 total"))
                                       .arg(fmt::bytes(space.bytesAvailable), fmt::bytes(space.bytesTotal));
            auto *spLbl = new WrapLabel(space.lowSpace ? QStringLiteral("⚠ ") + spText : spText, bak);
            spLbl->setWordWrap(true);
            spLbl->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
            spLbl->setAlignment(Qt::AlignLeft | Qt::AlignTop);
            if (space.lowSpace) {
                QPalette pal = spLbl->palette();
                pal.setColor(QPalette::WindowText, Theme::t().danger);
                spLbl->setPalette(pal);
            }
            bakGrid->addWidget(new QLabel(T("磁盘空间"), bak), bRow, 0, Qt::AlignLeft | Qt::AlignTop);
            bakGrid->addWidget(spLbl, bRow++, 1);
        }
        bakCol->addLayout(bakGrid);

        auto *bakRow = new QHBoxLayout;
        auto *manageBtn = new QPushButton(T("本地备份管理…"), bak);
        auto *nowBtn = new QPushButton(T("立即备份"), bak);
        auto *openBtn = new QPushButton(T("打开备份目录"), bak);
        openBtn->setEnabled(!cfg.directory.isEmpty());
        connect(manageBtn, &QPushButton::clicked, this, [this] { accept(); emit manageBackup(); });
        connect(nowBtn, &QPushButton::clicked, this, [this] { accept(); emit backupNow(); });
        connect(openBtn, &QPushButton::clicked, this, [dirPath = cfg.directory] {
            if (dirPath.isEmpty()) return;
            QDir().mkpath(dirPath);
            QDesktopServices::openUrl(QUrl::fromLocalFile(dirPath));
        });
        bakRow->addWidget(manageBtn);
        bakRow->addWidget(nowBtn);
        bakRow->addWidget(openBtn);
        bakRow->addStretch(1);
        bakCol->addLayout(bakRow);
        bakCol->addWidget(note(
            info.isRemote()
                ? T("在线笔记库默认关闭本地备份，可手动开启以定时将笔记与附件增量导出为 Markdown 留存本机，可直接浏览或手动导入恢复。")
                : T("本地笔记库默认开启定时增量 Markdown 备份，可直接浏览或手动导入恢复。"),
            bak));
        col->addWidget(bak);
    }

    auto *data = new QGroupBox(T("导入与导出"), w);
    auto *dataCol = new QVBoxLayout(data);
    auto *row = new QHBoxLayout;
    auto *importMd = new QPushButton(T("导入 Markdown…"), data);
    auto *importVaultButton = new QPushButton(T("从其他笔记库导入…"), data);
    auto *exportMd = new QPushButton(T("导出为 Markdown…"), data);
    connect(importMd, &QPushButton::clicked, this, [this] { accept(); emit importMarkdown(); });
    connect(importVaultButton, &QPushButton::clicked, this, [this] { accept(); emit importVault(); });
    connect(exportMd, &QPushButton::clicked, this, [this] { accept(); emit exportMarkdown(); });
    importVaultButton->setEnabled(vaults::list().size() > 1);
    row->addWidget(importMd);
    row->addWidget(importVaultButton);
    row->addWidget(exportMd);
    row->addStretch(1);
    dataCol->addLayout(row);
    dataCol->addWidget(note(info.isRemote() ? T("要把本地笔记库的内容放进这里：选择「从其他笔记库导入」。导入的笔记会像新写的笔记一样同步。")
                                            : T("本地笔记库不能改为同步。要同步这些笔记，先接入一个服务器笔记库，再从那里导入这个笔记库。"),
                           data));
    col->addWidget(data);

    auto *danger = new QGroupBox(T("移除"), w);
    auto *dangerCol = new QVBoxLayout(danger);
    const QString removeLabel = info.isRemote()
        ? T("退出登录并从本机移除…")
        : (info.hasCustomPath() ? T("移除或删除这个笔记库…") : T("删除这个笔记库…"));
    const QString removeNote = info.isRemote()
        ? T("会先同步，再结束登录，然后删除本机的笔记、附件缓存和令牌。服务器上的数据不受影响。")
        : (info.hasCustomPath()
               ? T("可以选择仅从列表移除（保留磁盘上的笔记库文件，日后可重新打开），或彻底删除笔记库文件。")
               : T("会永久删除这台电脑上这个笔记库的全部笔记和附件。"));
    auto *remove = new QPushButton(removeLabel, danger);
    connect(remove, &QPushButton::clicked, this, [this] { emit removeVault(); });
    dangerCol->addWidget(remove, 0, Qt::AlignLeft);
    dangerCol->addWidget(note(removeNote, danger));
    col->addWidget(danger);
    col->addStretch(1);
    scroll->setWidget(w);
    return scroll;
}

QWidget *SettingsDialog::accountTab() {
    auto *w = new QWidget;
    auto *col = new QVBoxLayout(w);
    col->setContentsMargins(20, 20, 20, 20);
    col->setSpacing(14);
    const User u = m_session->user();

    auto *profile = new QGroupBox(T("资料"), w);
    auto *form = new QFormLayout(profile);
    auto *display = new QLineEdit(u.displayName, profile);
    display->setMaxLength(64);
    auto *email = new QLineEdit(u.email, profile);
    auto *save = new QPushButton(T("保存"), profile);
    form->addRow(T("用户名"), new QLabel(u.username, profile));
    form->addRow(T("显示名称"), display);
    form->addRow(T("邮箱"), email);
    form->addRow(QString(), save);
    connect(save, &QPushButton::clicked, this, [this, display, email, save] {
        if (!m_session) return;
        save->setEnabled(false);
        // Both fields every time: a missing one would be cleared.
        m_session->api()->patchJson(QStringLiteral("/me"), QJsonObject{{"displayName", display->text().trimmed()}, {"email", email->text().trimmed()}}, this,
                                    [this, save](const ApiReply &r) {
                                        save->setEnabled(true);
                                        if (r.ok) {
                                            Toaster::show(T("已保存"));
                                            if (m_session) m_session->wake();
                                        } else {
                                            Toaster::show(authErrorText(r.status, r.error), true);
                                        }
                                    });
    });
    col->addWidget(profile);

    auto *pw = new QGroupBox(T("修改密码"), w);
    auto *pform = new QFormLayout(pw);
    auto *oldPw = new QLineEdit(pw);
    auto *newPw = new QLineEdit(pw);
    oldPw->setEchoMode(QLineEdit::Password);
    newPw->setEchoMode(QLineEdit::Password);
    auto *change = new QPushButton(T("修改密码"), pw);
    pform->addRow(T("当前密码"), oldPw);
    pform->addRow(T("新密码"), newPw);
    pform->addRow(QString(), change);
    pform->addRow(QString(), note(T("修改后其他设备需要重新登录。"), pw));
    connect(change, &QPushButton::clicked, this, [this, oldPw, newPw, change] {
        if (!m_session) return;
        if (newPw->text().size() < 8 || newPw->text().toUtf8().size() > 72) {
            Toaster::show(T("密码至少 8 个字符，最多 72 字节。"), true);
            return;
        }
        change->setEnabled(false);
        m_session->api()->postJson(QStringLiteral("/me/password"), QJsonObject{{"oldPassword", oldPw->text()}, {"newPassword", newPw->text()}}, this,
                                   [this, oldPw, newPw, change](const ApiReply &r) {
                                       change->setEnabled(true);
                                       if (!r.ok) {
                                           Toaster::show(r.status == 401 || r.status == 400 ? T("当前密码不正确。") : authErrorText(r.status, r.error), true);
                                           return;
                                       }
                                       const QString token = r.json.toObject().value(QLatin1String("token")).toString();
                                       if (m_session && !token.isEmpty()) {
                                           QString err;
                                           m_session->applySignIn(token, m_session->user(), &err);
                                       }
                                       oldPw->clear();
                                       newPw->clear();
                                       Toaster::show(T("密码已修改"));
                                   });
    });
    col->addWidget(pw);

    auto *links = new QHBoxLayout;
    auto *web = new QPushButton(T("打开网页版"), w);
    connect(web, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl(m_session->info().server)); });
    links->addWidget(web);
    if (u.role == QLatin1String("admin")) {
        auto *admin = new QPushButton(T("打开管理后台"), w);
        connect(admin, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl(m_session->info().server + QStringLiteral("/admin"))); });
        links->addWidget(admin);
    }
    links->addStretch(1);
    col->addLayout(links);
    col->addWidget(note(T("API 令牌、分享管理、备份等功能请在网页版设置中管理。"), w));
    col->addStretch(1);
    return w;
}

QWidget *SettingsDialog::aboutTab() {
    auto *w = new QWidget;
    auto *col = new QVBoxLayout(w);
    col->setContentsMargins(20, 28, 20, 20);
    col->setSpacing(10);
    col->addWidget(new LeafWidget(56, 1.0, w), 0, Qt::AlignHCenter);
    auto *name = new QLabel(T("常青笔记"), w);
    name->setFont(Theme::serif(24));
    name->setAlignment(Qt::AlignHCenter);
    col->addWidget(name);
    auto *tag = mutedLabel(T("本地优先的 Markdown 笔记") + QStringLiteral(" · ") + QStringLiteral("Qt " QT_VERSION_STR " · v" IVY_VERSION), w, 12.5);
    tag->setAlignment(Qt::AlignHCenter);
    col->addWidget(tag);
    m_checkUpdate = new QPushButton(T("检查更新"), w);
    m_checkUpdate->setObjectName(QStringLiteral("aboutCheckUpdate"));
    connect(m_checkUpdate, &QPushButton::clicked, this, [this] {
        setUpdateStatus(true);
        emit checkUpdates();
    });
    col->addWidget(m_checkUpdate, 0, Qt::AlignHCenter);
    m_updateStatus = mutedLabel(QString(), w, 12.0);
    m_updateStatus->setObjectName(QStringLiteral("aboutUpdateStatus"));
    m_updateStatus->setAlignment(Qt::AlignHCenter);
    m_updateStatus->setWordWrap(true);
    m_updateStatus->hide();
    col->addWidget(m_updateStatus);
    col->addSpacing(12);
    auto *body = note(T("笔记先保存在本机，再在后台同步。服务器端的数据由服务器加密保存；勾选「加密本机数据」后，这台电脑上的副本也会加密，密钥存放在系统钥匙串中。"), w);
    body->setAlignment(Qt::AlignHCenter);
    col->addWidget(body);
    col->addStretch(1);
    return w;
}

void SettingsDialog::setUpdateStatus(bool checking, const QString &message) {
    if (m_checkUpdate) {
        m_checkUpdate->setEnabled(!checking);
        m_checkUpdate->setText(checking ? T("正在检查更新…") : T("检查更新"));
    }
    if (m_updateStatus) {
        m_updateStatus->setText(message);
        m_updateStatus->setVisible(!message.isEmpty());
    }
}

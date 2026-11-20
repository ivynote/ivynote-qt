#include "ui/backupdialog.h"

#include "core/format.h"
#include "core/i18n.h"
#include "ui/dialogs.h"
#include "ui/theme.h"
#include "ui/widgets.h"
#include "vault/session.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {

QLabel *hintLabel(const QString &text, QWidget *parent) {
    auto *l = mutedLabel(text, parent, 12);
    l->setWordWrap(true);
    l->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    l->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    return l;
}

const QList<QPair<int, const char *>> kIntervals{
    {15, "每 15 分钟"},
    {30, "每 30 分钟"},
    {60, "每 1 小时"},
    {180, "每 3 小时"},
    {360, "每 6 小时"},
    {1440, "每天"},
};

} // namespace

LocalBackupDialog::LocalBackupDialog(Session *session, LocalBackupManager *manager, bool firstRun, QWidget *parent)
    : QDialog(parent), m_session(session), m_manager(manager), m_firstRun(firstRun) {
    setWindowTitle(firstRun ? T("本地笔记自动备份设置") : T("本地备份管理"));
    setMinimumWidth(640);

    const backup::Config cfg = manager ? manager->config()
                                       : (session ? backup::loadConfig(session->info()) : backup::Config{});

    auto *col = new QVBoxLayout(this);
    col->setContentsMargins(20, 18, 20, 18);
    col->setSpacing(14);

    if (firstRun) {
        auto *intro = new QLabel(
            T("当前为本地笔记库（不会同步到云端）。已为你默认开启定时增量 Markdown 备份，并自动选择了容量最大的可用磁盘位置。"
              "备份目录中的 .md 文件与附件可直接查看、二次处理，或在异常时重新导入。"),
            this);
        intro->setWordWrap(true);
        intro->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        intro->setFont(Theme::uiFont(13));
        col->addWidget(intro);
    }

    // --- Backup settings ---
    auto *cfgGroup = new QGroupBox(T("备份设置"), this);
    auto *cfgCol = new QVBoxLayout(cfgGroup);
    cfgCol->setSpacing(10);

    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(10);
    grid->setColumnStretch(1, 1);

    int row = 0;
    m_enabled = new QCheckBox(T("开启定时增量备份"), cfgGroup);
    m_enabled->setObjectName(QStringLiteral("backupEnabledCheck"));
    m_enabled->setChecked(cfg.enabled);
    grid->addWidget(m_enabled, row++, 1, Qt::AlignLeft);

    auto *dirTitle = new QLabel(T("备份目录"), cfgGroup);
    auto *dirCol = new QVBoxLayout;
    dirCol->setContentsMargins(0, 0, 0, 0);
    dirCol->setSpacing(6);
    m_dirEdit = new QLineEdit(QDir::toNativeSeparators(cfg.directory), cfgGroup);
    m_dirEdit->setObjectName(QStringLiteral("backupDirEdit"));
    m_dirEdit->setReadOnly(true);
    m_dirEdit->setToolTip(m_dirEdit->text());
    m_dirEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    dirCol->addWidget(m_dirEdit);

    auto *dirBtnRow = new QHBoxLayout;
    dirBtnRow->setContentsMargins(0, 0, 0, 0);
    dirBtnRow->setSpacing(6);
    m_browseBtn = new QPushButton(T("浏览…"), cfgGroup);
    m_browseBtn->setObjectName(QStringLiteral("backupBrowseBtn"));
    m_autoBtn = new QPushButton(T("自动选最大盘"), cfgGroup);
    m_autoBtn->setObjectName(QStringLiteral("backupAutoDirBtn"));
    m_openBtn = new QPushButton(T("打开目录"), cfgGroup);
    m_openBtn->setObjectName(QStringLiteral("backupOpenDirBtn"));
    dirBtnRow->addWidget(m_browseBtn);
    dirBtnRow->addWidget(m_autoBtn);
    dirBtnRow->addWidget(m_openBtn);
    dirBtnRow->addStretch(1);
    dirCol->addLayout(dirBtnRow);

    grid->addWidget(dirTitle, row, 0, Qt::AlignLeft | Qt::AlignTop);
    grid->addLayout(dirCol, row++, 1);

    auto *spaceTitle = new QLabel(T("磁盘空间"), cfgGroup);
    m_spaceLabel = mutedLabel(QString(), cfgGroup, 12);
    m_spaceLabel->setObjectName(QStringLiteral("backupSpaceLabel"));
    m_spaceLabel->setWordWrap(true);
    m_spaceLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    m_spaceLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    grid->addWidget(spaceTitle, row, 0, Qt::AlignLeft | Qt::AlignTop);
    grid->addWidget(m_spaceLabel, row++, 1);

    auto *intervalTitle = new QLabel(T("备份频率"), cfgGroup);
    m_intervalBox = new QComboBox(cfgGroup);
    m_intervalBox->setObjectName(QStringLiteral("backupIntervalBox"));
    int selectedIdx = 1; // 30 min default
    for (int i = 0; i < kIntervals.size(); ++i) {
        m_intervalBox->addItem(T(kIntervals[i].second), kIntervals[i].first);
        if (kIntervals[i].first == cfg.intervalMinutes) selectedIdx = i;
    }
    m_intervalBox->setCurrentIndex(selectedIdx);
    grid->addWidget(intervalTitle, row, 0, Qt::AlignLeft | Qt::AlignVCenter);
    grid->addWidget(m_intervalBox, row++, 1, Qt::AlignLeft);

    cfgCol->addLayout(grid);

    m_spaceWarning = new QLabel(cfgGroup);
    m_spaceWarning->setObjectName(QStringLiteral("backupSpaceWarning"));
    m_spaceWarning->setWordWrap(true);
    m_spaceWarning->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    m_spaceWarning->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_spaceWarning->setFont(Theme::uiFont(12.5, QFont::Medium));
    QPalette wp = m_spaceWarning->palette();
    wp.setColor(QPalette::WindowText, Theme::t().danger);
    m_spaceWarning->setPalette(wp);
    m_spaceWarning->hide();
    cfgCol->addWidget(m_spaceWarning);

    auto *hint = hintLabel(T("采用增量导出：仅写入发生变化的笔记与附件，未改动文件直接跳过；不会删除你在备份目录中自行存放的其他文件。"), cfgGroup);
    hint->setObjectName(QStringLiteral("backupHintLabel"));
    cfgCol->addWidget(hint);
    col->addWidget(cfgGroup);

    // --- Status & quick actions ---
    auto *statusGroup = new QGroupBox(T("备份状态与操作"), this);
    auto *statusCol = new QVBoxLayout(statusGroup);
    statusCol->setSpacing(8);

    m_statusLabel = new QLabel(statusGroup);
    m_statusLabel->setObjectName(QStringLiteral("backupStatusLabel"));
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    statusCol->addWidget(m_statusLabel);

    m_progress = new QProgressBar(statusGroup);
    m_progress->setRange(0, 100);
    m_progress->setTextVisible(true);
    m_progress->hide();
    statusCol->addWidget(m_progress);

    auto *actRow = new QHBoxLayout;
    actRow->setSpacing(8);
    m_backupNowBtn = new QPushButton(T("立即备份"), statusGroup);
    m_backupNowBtn->setObjectName(QStringLiteral("backupNowBtn"));
    m_importBtn = new QPushButton(T("从备份导入恢复…"), statusGroup);
    m_importBtn->setObjectName(QStringLiteral("backupImportBtn"));
    actRow->addWidget(m_backupNowBtn);
    actRow->addWidget(m_importBtn);
    actRow->addStretch(1);
    statusCol->addLayout(actRow);
    col->addWidget(statusGroup);

    // --- History ---
    auto *histGroup = new QGroupBox(T("最近备份记录"), this);
    auto *histCol = new QVBoxLayout(histGroup);
    m_historyList = new QListWidget(histGroup);
    m_historyList->setObjectName(QStringLiteral("backupHistoryList"));
    m_historyList->setMinimumHeight(firstRun ? 88 : 130);
    m_historyList->setSelectionMode(QAbstractItemView::NoSelection);
    histCol->addWidget(m_historyList);
    col->addWidget(histGroup, 1);

    // --- Bottom buttons ---
    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    if (firstRun) {
        auto *laterBtn = new QPushButton(T("稍后再说"), this);
        laterBtn->setObjectName(QStringLiteral("backupLaterBtn"));
        auto *confirmBtn = new QPushButton(T("确认并立即备份"), this);
        confirmBtn->setObjectName(QStringLiteral("backupConfirmBtn"));
        confirmBtn->setDefault(true);
        btnRow->addWidget(laterBtn);
        btnRow->addWidget(confirmBtn);
        connect(laterBtn, &QPushButton::clicked, this, [this] {
            if (m_manager) m_manager->updateConfig(gatherConfig(true));
            else if (m_session) backup::saveConfig(m_session->info().id, gatherConfig(true));
            accept();
        });
        connect(confirmBtn, &QPushButton::clicked, this, [this] {
            saveConfigAndRun(true);
        });
    } else {
        auto *saveBtn = new QPushButton(T("保存设置"), this);
        saveBtn->setObjectName(QStringLiteral("backupSaveBtn"));
        auto *closeBtn = new QPushButton(T("关闭"), this);
        closeBtn->setDefault(true);
        btnRow->addWidget(saveBtn);
        btnRow->addWidget(closeBtn);
        connect(saveBtn, &QPushButton::clicked, this, [this] {
            saveConfigAndRun(false);
        });
        connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    }
    col->addLayout(btnRow);

    // --- Wiring ---
    auto updateEnabledControls = [this] {
        const bool on = m_enabled->isChecked();
        m_intervalBox->setEnabled(on);
    };
    connect(m_enabled, &QCheckBox::toggled, this, updateEnabledControls);
    updateEnabledControls();

    connect(m_browseBtn, &QPushButton::clicked, this, [this] {
        const QString startDir = QFileInfo(selectedDirectory()).exists() ? selectedDirectory() : QDir::homePath();
        const QString picked = QFileDialog::getExistingDirectory(this, T("选择本地备份目录"), startDir);
        if (picked.isEmpty()) return;
        if (m_session && backup::isPathInsideVault(picked, m_session->info())) {
            QMessageBox::warning(this, T("备份目录无效"), T("备份目录不能位于笔记库数据目录内，请重新选择其他位置。"));
            return;
        }
        m_dirEdit->setText(QDir::toNativeSeparators(QDir::cleanPath(picked)));
        m_dirEdit->setToolTip(m_dirEdit->text());
        refreshSpaceInfo(true);
        refreshStatusAndHistory();
    });

    connect(m_autoBtn, &QPushButton::clicked, this, [this] {
        if (!m_session) return;
        const QString suggested = backup::suggestDefaultDirectory(m_session->info());
        m_dirEdit->setText(QDir::toNativeSeparators(suggested));
        m_dirEdit->setToolTip(m_dirEdit->text());
        refreshSpaceInfo(true);
        refreshStatusAndHistory();
    });

    connect(m_openBtn, &QPushButton::clicked, this, [this] {
        const QString dir = selectedDirectory();
        if (dir.isEmpty()) return;
        QDir().mkpath(dir);
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });

    connect(m_backupNowBtn, &QPushButton::clicked, this, [this] {
        if (!validateAndWarn(true)) return;
        if (m_manager) {
            m_manager->updateConfig(gatherConfig(true));
            m_manager->runNow(true);
        }
    });

    connect(m_importBtn, &QPushButton::clicked, this, [this] {
        const QString dir = selectedDirectory();
        if (dir.isEmpty() || !QDir(dir).exists()) {
            QMessageBox::information(this, T("从备份导入恢复"), T("当前备份目录尚不存在备份文件。"));
            return;
        }
        if (!dialogs::confirm(this, T("从备份导入恢复"),
                              T("将从当前备份目录导入所有 Markdown 笔记与附件到本笔记库（导入会创建新笔记，不会覆盖现有内容）。继续吗？"),
                              T("开始导入"))) {
            return;
        }
        emit importBackupRequested(dir);
        accept();
    });

    if (m_manager) {
        connect(m_manager, &LocalBackupManager::backupStarted, this, [this](bool) {
            m_backupNowBtn->setEnabled(false);
            m_progress->setRange(0, 0);
            m_progress->setFormat(T("正在准备备份…"));
            m_progress->show();
        });
        connect(m_manager, &LocalBackupManager::backupProgress, this, [this](int done, int total, const QString &what) {
            m_progress->setRange(0, std::max(1, total));
            m_progress->setValue(std::clamp(done, 0, std::max(1, total)));
            m_progress->setFormat(what + QStringLiteral(" (%1/%2)").arg(done).arg(std::max(1, total)));
        });
        connect(m_manager, &LocalBackupManager::backupFinished, this, [this](const backup::RunRecord &) {
            m_progress->hide();
            m_backupNowBtn->setEnabled(true);
            refreshSpaceInfo(false);
            refreshStatusAndHistory();
        });
    }

    refreshSpaceInfo(false);
    refreshStatusAndHistory();
    adjustSize();
}

QString LocalBackupDialog::selectedDirectory() const {
    return QDir::cleanPath(QDir::fromNativeSeparators(m_dirEdit->text().trimmed()));
}

void LocalBackupDialog::refreshSpaceInfo(bool popupIfLow) {
    const QString dir = selectedDirectory();
    const qint64 vaultBytes = m_session ? backup::estimateVaultBytes(m_session->info()) : 0;
    m_space = backup::checkSpace(dir, vaultBytes);

    if (!m_space.valid) {
        m_spaceLabel->setText(T("无法检测该目录所在磁盘的可用空间。"));
        m_spaceWarning->setText(T("⚠ 目标路径不可访问或磁盘未就绪，请检查目录。"));
        m_spaceWarning->show();
        return;
    }

    const QString info = (i18n::isZh()
                              ? QStringLiteral("所在磁盘 %1 · 可用 %2 / 共 %3")
                              : QStringLiteral("Disk %1 · %2 free / %3 total"))
                             .arg(m_space.mountRoot, fmt::bytes(m_space.bytesAvailable), fmt::bytes(m_space.bytesTotal));
    m_spaceLabel->setText(info);

    const bool writable = backup::isDirectoryWritable(dir);
    if (!writable) {
        m_spaceWarning->setText(T("⚠ 当前用户对该目录没有写入权限，请选择其他目录。"));
        m_spaceWarning->show();
        return;
    }

    if (m_space.criticalSpace) {
        const QString msg = (i18n::isZh()
                                 ? QStringLiteral("⚠ 磁盘可用空间严重不足（仅剩 %1，至少需要 %2），自动备份将会被中止，请更换空间更大的磁盘。")
                                 : QStringLiteral("⚠ Disk space is critically low (%1 free, need at least %2). Backups will fail; please choose a larger disk."))
                                .arg(fmt::bytes(m_space.bytesAvailable), fmt::bytes(m_space.criticalMinBytes));
        m_spaceWarning->setText(msg);
        m_spaceWarning->show();
        if (popupIfLow) QMessageBox::warning(this, T("磁盘空间不足"), msg);
    } else if (m_space.lowSpace) {
        const QString msg = (i18n::isZh()
                                 ? QStringLiteral("⚠ 所选目录所在磁盘可用空间较小（剩余 %1，建议保留至少 %2），建议更换到空间更充足的磁盘。")
                                 : QStringLiteral("⚠ Selected disk is low on free space (%1 free; at least %2 recommended). Consider choosing a larger disk."))
                                .arg(fmt::bytes(m_space.bytesAvailable), fmt::bytes(m_space.recommendedMinBytes));
        m_spaceWarning->setText(msg);
        m_spaceWarning->show();
        if (popupIfLow) QMessageBox::warning(this, T("磁盘空间较小"), msg);
    } else {
        m_spaceWarning->hide();
    }
}

void LocalBackupDialog::refreshStatusAndHistory() {
    const QString dir = selectedDirectory();
    const backup::ManifestSummary sum = backup::readManifestSummary(dir);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    if (!sum.exists || sum.lastBackupAt == 0) {
        m_statusLabel->setText(T("尚未执行过备份。点击「立即备份」可立刻生成一份完整 Markdown 备份。"));
    } else {
        m_statusLabel->setText(
            (i18n::isZh()
                 ? QStringLiteral("上次备份：%1 · 已备份 %2 篇笔记、%3 个附件")
                 : QStringLiteral("Last backup: %1 · %2 notes, %3 attachments"))
                .arg(fmt::fullTime(sum.lastBackupAt, now))
                .arg(sum.totalNotes)
                .arg(sum.totalAttachments));
    }

    m_importBtn->setEnabled(QDir(dir).exists());
    m_backupNowBtn->setEnabled(!m_manager || !m_manager->isRunning());

    m_historyList->clear();
    if (sum.history.isEmpty()) {
        auto *empty = new QListWidgetItem(T("暂无备份记录"), m_historyList);
        empty->setForeground(Theme::t().muted);
        return;
    }
    for (const backup::RunRecord &r : sum.history) {
        const QString when = fmt::fullTime(r.timestamp, now);
        const QString kind = r.manual ? T("手动") : T("自动");
        const QString status = r.ok ? QStringLiteral("✓") : QStringLiteral("✕");
        const QString text = QStringLiteral("%1  [%2]  %3  —  %4").arg(status, kind, when, r.summary);
        auto *item = new QListWidgetItem(text, m_historyList);
        item->setForeground(r.ok ? Theme::t().ink2 : Theme::t().danger);
    }
}

bool LocalBackupDialog::validateAndWarn(bool allowLowSpaceConfirm) {
    const QString dir = selectedDirectory();
    if (dir.isEmpty()) {
        QMessageBox::warning(this, T("备份目录无效"), T("请先选择一个备份目录。"));
        return false;
    }
    if (m_session && backup::isPathInsideVault(dir, m_session->info())) {
        QMessageBox::warning(this, T("备份目录无效"), T("备份目录不能位于笔记库数据目录内，请重新选择其他位置。"));
        return false;
    }
    refreshSpaceInfo(false);
    if (!backup::isDirectoryWritable(dir)) {
        QMessageBox::warning(this, T("备份目录不可写"), T("当前用户无法写入所选目录，请更换有写入权限的目录。"));
        return false;
    }
    if (m_space.valid && m_space.criticalSpace) {
        QMessageBox::warning(this, T("磁盘空间不足"),
                             (i18n::isZh()
                                  ? QStringLiteral("所选目录所在磁盘仅剩 %1，空间严重不足，无法启用备份。请更换其他磁盘。")
                                  : QStringLiteral("Selected disk only has %1 free. Please choose another disk."))
                                 .arg(fmt::bytes(m_space.bytesAvailable)));
        return false;
    }
    if (allowLowSpaceConfirm && m_space.valid && m_space.lowSpace) {
        const auto res = QMessageBox::warning(
            this, T("磁盘可用空间较小"),
            (i18n::isZh()
                 ? QStringLiteral("所选目录所在磁盘可用空间较小（剩余 %1，建议至少 %2）。是否仍使用此目录？")
                 : QStringLiteral("Selected disk is low on free space (%1 free; %2 recommended). Use this folder anyway?"))
                .arg(fmt::bytes(m_space.bytesAvailable), fmt::bytes(m_space.recommendedMinBytes)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (res != QMessageBox::Yes) return false;
    }
    return true;
}

backup::Config LocalBackupDialog::gatherConfig(bool markConfigured) const {
    backup::Config cfg;
    cfg.enabled = m_enabled->isChecked();
    cfg.configured = markConfigured;
    cfg.directory = selectedDirectory();
    cfg.intervalMinutes = m_intervalBox->currentData().toInt();
    if (cfg.intervalMinutes < 5) cfg.intervalMinutes = backup::kDefaultIntervalMinutes;
    return cfg;
}

void LocalBackupDialog::saveConfigAndRun(bool runBackup) {
    if (m_enabled->isChecked() || runBackup) {
        if (!validateAndWarn(true)) return;
    }
    const backup::Config cfg = gatherConfig(true);
    if (m_manager) {
        m_manager->updateConfig(cfg);
        if (runBackup && cfg.enabled) m_manager->runNow(true);
    } else if (m_session) {
        backup::saveConfig(m_session->info().id, cfg);
    }
    accept();
}

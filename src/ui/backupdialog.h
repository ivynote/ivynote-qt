// Local Markdown backup configuration & management dialog (local vaults only).
#pragma once

#include "vault/backup.h"

#include <QDialog>
#include <QPointer>

class Session;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;

class LocalBackupDialog : public QDialog {
    Q_OBJECT
public:
    LocalBackupDialog(Session *session, LocalBackupManager *manager, bool firstRun = false, QWidget *parent = nullptr);

    // Exposed for UI tests / diagnostics.
    QString selectedDirectory() const;
    backup::SpaceCheck currentSpaceCheck() const { return m_space; }

signals:
    void importBackupRequested(const QString &dir);

private:
    void refreshSpaceInfo(bool popupIfLow);
    void refreshStatusAndHistory();
    bool validateAndWarn(bool allowLowSpaceConfirm);
    backup::Config gatherConfig(bool markConfigured) const;
    void saveConfigAndRun(bool runBackup);

    QPointer<Session> m_session;
    QPointer<LocalBackupManager> m_manager;
    bool m_firstRun = false;
    backup::SpaceCheck m_space;

    QCheckBox *m_enabled = nullptr;
    QLineEdit *m_dirEdit = nullptr;
    QPushButton *m_browseBtn = nullptr;
    QPushButton *m_autoBtn = nullptr;
    QPushButton *m_openBtn = nullptr;
    QLabel *m_spaceLabel = nullptr;
    QLabel *m_spaceWarning = nullptr;
    QComboBox *m_intervalBox = nullptr;
    QLabel *m_statusLabel = nullptr;
    QProgressBar *m_progress = nullptr;
    QPushButton *m_backupNowBtn = nullptr;
    QPushButton *m_importBtn = nullptr;
    QListWidget *m_historyList = nullptr;
};

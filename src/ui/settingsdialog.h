// Preferences of this device and the open vault.
#pragma once

#include <QDialog>
#include <QObject>
#include <QPointer>

class Session;
class QLabel;
class QPushButton;
class QTabWidget;

enum class OpenMode { Live = 0, Source = 1, Split = 2, Read = 3 };

// Device-wide preferences, kept in QSettings.
class Prefs : public QObject {
    Q_OBJECT
public:
    static Prefs *instance();
    int theme() const;        // Theme::Mode
    int lang() const;         // i18n::Lang
    int fontSize() const;     // 13-22
    int editorMode() const;   // OpenMode (0=Live, 1=Source, 2=Split, 3=Read)
    bool reduceMotion() const;
    QString defaultNoteFolder() const;
    QString defaultNoteTitle() const;
    bool attachmentCacheLocalFirst() const;
    qint64 attachmentCacheMaxBytes() const;
    void setTheme(int v);
    void setLang(int v);
    void setFontSize(int v);
    void setEditorMode(int v);
    void setReduceMotion(bool v);
    void setDefaultNoteFolder(const QString &v);
    void setDefaultNoteTitle(const QString &v);
    void setAttachmentCacheLocalFirst(bool v);
    void setAttachmentCacheMaxBytes(qint64 v);

signals:
    void changed();

private:
    using QObject::QObject;
};

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    SettingsDialog(Session *session, QWidget *parent = nullptr);
    void showVaultTab();
    void showAboutTab();
    void setUpdateStatus(bool checking, const QString &message = {});

signals:
    void importMarkdown();
    void importVault();
    void exportMarkdown();
    void manageBackup();
    void backupNow();
    void checkUpdates();
    void removeVault();
    void vaultRenamed();
    void prefsSynced();

private:
    QWidget *generalTab();
    QWidget *vaultTab();
    QWidget *accountTab();
    QWidget *aboutTab();

    QPointer<Session> m_session;
    QTabWidget *m_tabs;
    QWidget *m_vault = nullptr;
    QWidget *m_about = nullptr;
    QPushButton *m_checkUpdate = nullptr;
    QLabel *m_updateStatus = nullptr;
};

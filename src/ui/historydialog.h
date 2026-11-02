// Revision history of a note (online): list, preview, restore. Restoring goes
// through a normal local edit, so it syncs like one and the replaced text
// stays in the history.
#pragma once

#include <QDialog>
#include <QPointer>

class Session;
class QListWidget;
class QTextBrowser;
class QPushButton;
class QLabel;
class QTabBar;

class HistoryDialog : public QDialog {
    Q_OBJECT
public:
    HistoryDialog(Session *session, const QString &noteId, QWidget *parent = nullptr);

private:
    void load();
    void showRevision(int row);
    void render();

    QPointer<Session> m_session;
    QString m_noteId;
    QListWidget *m_list;
    QTextBrowser *m_view;
    QTabBar *m_mode;
    QLabel *m_status;
    QPushButton *m_restore;
    QString m_title, m_body;
};

class ShareDialog : public QDialog {
    Q_OBJECT
public:
    ShareDialog(Session *session, const QString &noteId, QWidget *parent = nullptr);

private:
    void load();
    void put(bool enabled, qint64 expiresAt, const QString *password);
    void apply(const class QJsonObject &share);
    void copyMarkdown();
    void copyRich();
    void exportFile();

    QPointer<Session> m_session;
    QString m_noteId;
    class QCheckBox *m_enabled;
    class QLineEdit *m_link;
    class QComboBox *m_expiry;
    class QLineEdit *m_password;
    QLabel *m_info;
    QLabel *m_status;
    qint64 m_expiresAt = 0;
    bool m_hasPassword = false;
    bool m_loading = false;
};

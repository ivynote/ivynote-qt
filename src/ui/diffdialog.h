// Visual diff dialog for conflict copies and comparing any two notes,
// aligned with web/Tauri's NoteDiffDialog.
#pragma once

#include <QDialog>
#include <QPointer>
#include <QString>

class Session;
class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QTextBrowser;
class QToolButton;

class NoteDiffDialog : public QDialog {
    Q_OBJECT
public:
    NoteDiffDialog(Session *session, const QString &baseNoteId, const QString &initialTargetId = {}, QWidget *parent = nullptr);

signals:
    void openNoteRequested(const QString &noteId);

private:
    void pickTarget(const QString &id);
    void showPicker();
    void refreshPicker();
    void refreshDiff();
    void appendRight();
    void replaceWithRight();
    void trashRight();

    QPointer<Session> m_session;
    QString m_baseId;
    QString m_targetId;
    bool m_unified = false;
    bool m_changesOnly = false;

    QLabel *m_baseTitle = nullptr;
    QLabel *m_targetTitle = nullptr;
    QLabel *m_conflictBadge = nullptr;
    QPushButton *m_switchBtn = nullptr;

    QWidget *m_controlsBar = nullptr;
    QLabel *m_stats = nullptr;
    QCheckBox *m_onlyChangesBox = nullptr;
    QToolButton *m_splitBtn = nullptr;
    QToolButton *m_unifiedBtn = nullptr;

    QStackedWidget *m_stack = nullptr;
    QWidget *m_pickerPage = nullptr;
    QLineEdit *m_pickerSearch = nullptr;
    QListWidget *m_pickerList = nullptr;
    QTextBrowser *m_diffView = nullptr;
    QWidget *m_identicalPage = nullptr;

    QWidget *m_actionsBar = nullptr;
    QPushButton *m_openTargetBtn = nullptr;
    QPushButton *m_appendBtn = nullptr;
    QPushButton *m_replaceBtn = nullptr;
    QPushButton *m_trashBtn = nullptr;
};

// The window's chrome besides the notebook shelf: the side panels (agenda,
// media library), their shared header, and the sync indicator in the status bar.
#pragma once

#include "core/tasks.h"

#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QToolButton>
#include <QWidget>
#include <optional>

class Session;
class SyncEngine;
class QLabel;
class QLineEdit;
class QListWidget;
class QComboBox;
class QStackedLayout;

// One row of title and one of detail, drawn the way the shelf draws its rows
// so the side panels read as one family.
class PanelHeader : public QWidget {
    Q_OBJECT
public:
    explicit PanelHeader(const QString &title, QWidget *parent = nullptr);
    void setCount(int n); // shown to the right of the title; hidden when negative
    QLayout *actions() const;

private:
    QLabel *m_count;
};

class AgendaPanel : public QWidget {
    Q_OBJECT
public:
    explicit AgendaPanel(QWidget *parent = nullptr);
    void setSession(Session *session);
    void refresh();
    void setShowDone(bool show);
    bool showDone() const { return m_showDone; }

signals:
    void taskActivated(const QString &noteId, int line);
    void taskToggled(const QString &noteId, int line, const QString &rawLine);
    void taskDueChanged(const QString &noteId, int line, const QString &rawLine,
                        const std::optional<tasks::Due> &due, const QString &repeat);
    void badgeChanged(int count, bool hasOverdue);

private:
    void toggleBranch(const QString &key);
    void showTaskMenu(const QPoint &pos);
    void loadState();
    void saveState() const;

    QPointer<Session> m_session;
    PanelHeader *m_header;
    QToolButton *m_doneToggle;
    QLabel *m_summary;
    QListWidget *m_list;
    QLabel *m_empty;
    QLabel *m_encryptedHint;
    QStackedLayout *m_pages;
    QTimer m_refresh;
    bool m_showDone = false;
    QSet<QString> m_toggledBranches; // overrides Branch::openByDefault()
};

class MediaPanel : public QWidget {
    Q_OBJECT
public:
    explicit MediaPanel(QWidget *parent = nullptr);
    void setSession(Session *session);
    void refresh();

private:
    void menu(const QPoint &pos);
    void open();

    QPointer<Session> m_session;
    PanelHeader *m_header;
    QLineEdit *m_filter;
    QComboBox *m_kind;
    QListWidget *m_list;
    QLabel *m_empty;
    QStackedLayout *m_pages;
    QTimer m_refresh;
};

// Sync state of the open vault, one line in the status bar. Clicking it
// syncs now, or asks for the password again when the session has lapsed.
class SyncIndicator : public QToolButton {
    Q_OBJECT
public:
    explicit SyncIndicator(QWidget *parent = nullptr);
    void setSession(Session *session);

private:
    void refreshState();

    QPointer<Session> m_session;
    SyncEngine *m_engine = nullptr;
    QTimer m_clock;
};

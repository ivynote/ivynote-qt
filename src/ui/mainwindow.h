// The window: shelf on the desk, the note on its paper, menus and flows that
// span both (vaults, import/export, sign-in).
#pragma once

#include "core/tasks.h"

#include <QMainWindow>
#include <QPointer>
#include <optional>

class Session;
class LocalBackupManager;
class Shelf;
class AgendaPanel;
class MediaPanel;
class SyncIndicator;
class NotePane;
class Divider;
class QStackedWidget;
class QAction;
class QActionGroup;
class QMenu;
class QTabBar;
class QToolBar;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;

    // Opens the last vault, or makes a first local one so the app is usable at once.
    void start();

protected:
    void closeEvent(QCloseEvent *e) override;
    void changeEvent(QEvent *e) override;

private:
    bool openVault(const QString &id);
    void closeVault();
    void buildMenus();
    void rebuildVaultMenu();
    void showVaultMenu(const QPoint &global);
    void addVault(bool server = false);
    void createLocalVault();
    void openExistingVault();
    void newNote(const QString &folderId);
    void openNote(const QString &id);
    void openNoteInMode(const QString &id, bool forEditing);
    void openDiff(const QString &baseId, const QString &otherId = QString());
    void moveNote(const QString &id);
    void moveNotes(const QStringList &ids);
    void dropEmptyDraft(const QString &next);
    void duplicateNote(const QString &id);
    void emptyTrash();
    void showSearch(const QString &initial = {});
    enum SettingsPage { GeneralPage, VaultPage, AboutPage };
    void showSettings(SettingsPage page = GeneralPage);
    void showBackupManager(bool firstRun = false);
    void signIn();
    void toggleShelf();
    void applyPrefs();
    void applyOpenMode(int mode);
    void importMarkdown();
    void importVault();
    void exportMarkdown();
    void removeVault();
    void runJob(class TransferJob *job, const QString &title);
    void syncPrefsFromServer();
    void updateTitle();
    enum Page { NotesPage, AgendaPage, MediaPage };
    void showPage(Page page, bool focus = true);
    void syncRail();
    void refreshRailIcons();
    void openTask(const QString &noteId, int line);
    void toggleAgendaTask(const QString &noteId, int line, const QString &rawLine);
    void rescheduleAgendaTask(const QString &noteId, int line, const QString &rawLine,
                              const std::optional<tasks::Due> &due, const QString &repeat);
    void refreshNoteTabs();

    QPointer<Session> m_session;
    LocalBackupManager *m_backup = nullptr;
    Divider *m_split;
    QStackedWidget *m_side;
    Shelf *m_shelf;
    AgendaPanel *m_agenda;
    MediaPanel *m_media;
    SyncIndicator *m_sync;
    QToolBar *m_rail = nullptr;
    QList<QAction *> m_railPages;
    QAction *m_railSettings = nullptr;
    int m_agendaBadgeCount = 0;
    bool m_agendaBadgeOverdue = false;
    int m_appliedOpenMode = -1;
    Page m_page = NotesPage;
    QStackedWidget *m_stack;
    NotePane *m_pane;
    QTabBar *m_tabs;
    QWidget *m_empty;
    QString m_draftId;    // a note made with "new note"; dropped if left untouched
    QString m_draftTitle; // initial templated title of m_draftId
    bool m_shelfCollapsed = false;
    bool m_signInOpen = false;
    bool m_signInAsked = false; // prompt by itself once per vault; later via the status line
    QMenu *m_vaultMenu = nullptr;
    QActionGroup *m_modeGroup = nullptr;
    QAction *m_readAction = nullptr;
    QList<QAction *> m_noteActions;
    QList<QAction *> m_remoteActions;
};

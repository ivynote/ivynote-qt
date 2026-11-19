// The window: shelf on the desk, the note on its paper, menus and flows that
// span both (vaults, import/export, sign-in).
#pragma once

#include <QMainWindow>
#include <QPointer>

class Session;
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
    void newNote(const QString &folderId);
    void openNote(const QString &id);
    void moveNote(const QString &id);
    void moveNotes(const QStringList &ids);
    void dropEmptyDraft(const QString &next);
    void duplicateNote(const QString &id);
    void emptyTrash();
    void showSearch(const QString &initial = {});
    enum SettingsPage { GeneralPage, VaultPage, AboutPage };
    void showSettings(SettingsPage page = GeneralPage);
    void signIn();
    void toggleShelf();
    void applyPrefs();
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
    void openTask(const QString &noteId, int line);
    void refreshNoteTabs();

    QPointer<Session> m_session;
    Divider *m_split;
    QStackedWidget *m_side;
    Shelf *m_shelf;
    AgendaPanel *m_agenda;
    MediaPanel *m_media;
    SyncIndicator *m_sync;
    QToolBar *m_rail = nullptr;
    QList<QAction *> m_railPages;
    Page m_page = NotesPage;
    QStackedWidget *m_stack;
    NotePane *m_pane;
    QTabBar *m_tabs;
    QWidget *m_empty;
    QString m_draftId; // a note made with "new note"; dropped if left empty
    bool m_shelfCollapsed = false;
    bool m_signInOpen = false;
    bool m_signInAsked = false; // prompt by itself once per vault; later via the status line
    QMenu *m_vaultMenu = nullptr;
    QActionGroup *m_modeGroup = nullptr;
    QList<QAction *> m_noteActions;
    QList<QAction *> m_remoteActions;
};

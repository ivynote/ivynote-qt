// The window: shelf on the desk, the note on its paper, menus and flows that
// span both (vaults, import/export, sign-in).
#pragma once

#include <QMainWindow>
#include <QPointer>

class Session;
class Shelf;
class NotePane;
class QSplitter;
class QStackedWidget;
class QAction;
class QActionGroup;
class QMenu;

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
    void duplicateNote(const QString &id);
    void emptyTrash();
    void showSearch(const QString &initial = {});
    void showSettings(bool vaultTab = false);
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

    QPointer<Session> m_session;
    QSplitter *m_split;
    Shelf *m_shelf;
    QStackedWidget *m_stack;
    NotePane *m_pane;
    QWidget *m_empty;
    bool m_shelfCollapsed = false;
    bool m_signInOpen = false;
    bool m_signInAsked = false; // prompt by itself once per vault; later via the status line
    QMenu *m_vaultMenu = nullptr;
    QActionGroup *m_modeGroup = nullptr;
    QList<QAction *> m_noteActions;
    QList<QAction *> m_remoteActions;
};

// The note on its paper (guide 11.3): breadcrumb, title, meta and tags, the
// editor in one of three modes, outline and backlinks.
#pragma once

#include "core/notelock.h"
#include "ui/highlighter.h"

#include <QHash>
#include <QPointer>
#include <QTimer>
#include <QWidget>

class Session;
class SyncEngine;
class MarkdownEditor;
class QLabel;
class QLineEdit;
class QListWidget;
class QSplitter;
class QTextEdit;
class MarkdownView;
class QToolButton;
class QHBoxLayout;
class QStackedWidget;
class IconButton;
class PillButton;
class FlowRow;

enum class EditorMode { Live, Source, Split };

class NotePane : public QWidget {
    Q_OBJECT
public:
    explicit NotePane(QWidget *parent = nullptr);

    void setSession(Session *session);
    void setNote(const QString &id);
    QString noteId() const { return m_id; }
    void flush(); // hands unsaved text to the engine
    void syncBodyFromEngine();
    void focusEditor();
    void focusTitle();
    void showFind();
    void showReplace();
    void setEditorMode(EditorMode mode);
    EditorMode editorMode() const { return m_mode; }
    void setReading(bool reading);
    void toggleReading() { setReading(!m_reading); }
    bool isReading() const { return m_reading; }
    void setFontSize(int px);
    void toggleOutline();
    void insertFiles(const QStringList &paths);
    void pickFiles();
    void pickTags();
    void showInfo();
    MarkdownEditor *editor() const { return m_editor; }
    MarkdownView *preview() const { return m_preview; }
    bool isLocked() const { return m_locked; }       // protected and not unlocked
    bool isProtected() const { return m_locked || m_sealed; }
    void lockNow() { relock(); }

signals:
    void openNote(const QString &id);
    void moveRequested(const QString &id);
    void historyRequested(const QString &id);
    void shareRequested(const QString &id);
    void diffRequested(const QString &noteId, const QString &otherId = QString());
    void editorModeChanged(EditorMode mode);
    void readingChanged(bool reading);
    void duplicateRequested(const QString &id);

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
    void placeTopBar();
    void applyViewState();
    QTextEdit *activeTextView() const;
    void refresh();          // meta, breadcrumb, banner from the engine
    void refreshIndex();     // titles for links and completion; backlinks
    void refreshPreview();
    void refreshOutline();
    void findNext(bool backward = false);
    void centerMatch();
    void refreshFind();
    void replaceMatches(bool all);
    void scheduleBody();
    void openLink(const QString &target, bool wiki);
    void addImage(const QImage &img);
    void attach(const QByteArray &data, const QString &name, const QString &mime);
    void showMoreMenu();
    QString resolveTitle(const QString &title) const;
    void alignHead();              // title and meta on the editor's text column
    void syncScroll(bool fromEditor); // split view: the other side follows
    void buildScrollMap();

    // Password-protected notes (guide 8.4): the editor shows plaintext only
    // while unlocked; the engine only ever gets the sealed body.
    void loadBody(const QString &body);   // plain text, unlocked text or the lock panel
    void showLockPanel(bool locked, const QString &hint = {});
    void tryUnlock();
    void relock();                        // forget the key, show the lock panel
    void protectNote(bool change);
    void removeProtection();
    void reloadFromEngine();
    void onRemoteBody(const QString &body);

    QPointer<Session> m_session;
    SyncEngine *m_engine = nullptr;
    QString m_id;
    QString m_conflictTargetId;
    EditorMode m_mode = EditorMode::Live;
    bool m_reading = false;
    bool m_outlineOpen = false;
    bool m_bodyDirty = false;
    bool m_titleDirty = false;
    QHash<QString, QString> m_titleIndex; // normalized title -> newest live note id

    QWidget *m_findBar;
    QLineEdit *m_findText;
    QWidget *m_replaceRow;
    QLineEdit *m_replaceText;
    QToolButton *m_replaceOne, *m_replaceAll;
    QLabel *m_findCount;
    QToolButton *m_findCase;
    QWidget *m_topBar;
    QLabel *m_pendingDot;
    QLabel *m_savedLabel;
    IconButton *m_insertButton, *m_dueButton, *m_readButton, *m_diffButton, *m_shareButton, *m_outlineButton, *m_moreButton;
    QWidget *m_banner;
    QWidget *m_conflictBanner;
    QLabel *m_conflictLabel;
    QWidget *m_headHost;
    QWidget *m_head;
    QWidget *m_body;
    QLineEdit *m_title;
    QLabel *m_meta;
    FlowRow *m_tagsRow;
    QSplitter *m_split;
    MarkdownEditor *m_editor;
    MarkdownView *m_preview;
    QListWidget *m_outline;
    QToolButton *m_backlinksButton;
    QListWidget *m_backlinks;
    QTimer m_bodyTimer, m_titleTimer, m_previewTimer, m_indexTimer, m_savedTimer, m_lockTimer;
    int m_uploading = 0;
    bool m_syncing = false;
    QList<QPair<qreal, qreal>> m_scrollMap; // editor y -> preview y, both increasing

    bool m_sealed = false;    // an unlocked protected note: saves are sealed with m_lockKey
    bool m_locked = false;    // a protected note without a key: the lock panel is up
    bool m_unlocking = false; // key derivation in flight
    bool m_protecting = false;
    notelock::Key m_lockKey;
    QWidget *m_lockPanel;
    QLabel *m_lockIcon, *m_lockHint, *m_lockError;
    QLineEdit *m_lockPassword;
    PillButton *m_lockButton;
};

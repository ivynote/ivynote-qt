// The note on its paper (guide 11.3): breadcrumb, title, meta and tags, the
// editor in one of three modes, outline and backlinks.
#pragma once

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
class MarkdownView;
class QToolButton;
class QHBoxLayout;
class QStackedWidget;
class IconButton;
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
    void focusEditor();
    void focusTitle();
    void showFind();
    void setEditorMode(EditorMode mode);
    EditorMode editorMode() const { return m_mode; }
    void setFontSize(int px);
    void setShelfCollapsed(bool collapsed);
    void toggleOutline();
    void insertFiles(const QStringList &paths);
    void pickFiles();
    void pickTags();
    MarkdownEditor *editor() const { return m_editor; }

signals:
    void openNote(const QString &id);
    void revealFolder(const QString &folderId);
    void moveRequested(const QString &id);
    void historyRequested(const QString &id);
    void shareRequested(const QString &id);
    void expandShelfRequested();
    void editorModeChanged(EditorMode mode);
    void duplicateRequested(const QString &id);

private:
    void refresh();          // meta, breadcrumb, banner from the engine
    void refreshIndex();     // titles for links and completion; backlinks
    void refreshPreview();
    void refreshOutline();
    void findNext(bool backward = false);
    void refreshFind();
    void scheduleBody();
    void openLink(const QString &target, bool wiki);
    void addImage(const QImage &img);
    void attach(const QByteArray &data, const QString &name, const QString &mime);
    void showMoreMenu();
    QString resolveTitle(const QString &title) const;
    void alignHead();              // title and meta on the editor's text column
    void syncScroll(bool fromEditor); // split view: the other side follows
    void buildScrollMap();

    QPointer<Session> m_session;
    SyncEngine *m_engine = nullptr;
    QString m_id;
    EditorMode m_mode = EditorMode::Live;
    bool m_shelfCollapsed = false;
    bool m_outlineOpen = false;
    bool m_bodyDirty = false;
    bool m_titleDirty = false;
    QHash<QString, QString> m_titleIndex; // normalized title -> newest live note id

    QWidget *m_findBar;
    QLineEdit *m_findText;
    QLabel *m_findCount;
    QToolButton *m_findCase;
    QWidget *m_topBar;
    IconButton *m_expandShelf;
    QHBoxLayout *m_crumbs;
    QLabel *m_pendingDot;
    QLabel *m_savedLabel;
    IconButton *m_shareButton, *m_outlineButton, *m_moreButton;
    QWidget *m_banner;
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
    QTimer m_bodyTimer, m_titleTimer, m_previewTimer, m_indexTimer, m_savedTimer;
    int m_uploading = 0;
    bool m_syncing = false;
    QList<QPair<qreal, qreal>> m_scrollMap; // editor y -> preview y, both increasing
};

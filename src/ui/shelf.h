// The shelf: vault switcher, the notebook tree (guide 11.2) and sync status.
#pragma once

#include "core/tree.h"

#include <QAbstractListModel>
#include <QListView>
#include <QPointer>
#include <QSet>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVariantAnimation>
#include <QWidget>

class Session;
class SyncEngine;
class QLabel;
class QToolButton;
class IconButton;

// A row copied out of the engine: safe to keep across event-loop turns.
struct ShelfRow {
    QString key;
    TreeRow::Kind kind = TreeRow::NoteRow;
    int depth = 0;
    QString parent;
    QString label;
    QString icon;
    int count = 0;
    bool expandable = false;
    bool open = false;
    bool gap = false;
    bool trash = false;
    bool pinned = false;
    bool pending = false;
    bool untitled = false;
    QString dropFolder;
    QString id; // note, folder or tag id
    QString tagColor;
};

class ShelfModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum { RowRole = Qt::UserRole + 1 };
    using QAbstractListModel::QAbstractListModel;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;
    Qt::DropActions supportedDropActions() const override { return Qt::MoveAction; }
    Qt::DropActions supportedDragActions() const override { return Qt::MoveAction; }

    void setRows(const QList<ShelfRow> &rows);
    const ShelfRow &row(int i) const { return m_rows[i]; }
    int indexOfKey(const QString &key) const;
    int indexOfNote(const QString &noteId) const;
    const QList<ShelfRow> &rows() const { return m_rows; }

private:
    QList<ShelfRow> m_rows;
};

class ShelfDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &opt, const QModelIndex &index) const override;

    bool isSelected(const ShelfRow &r) const;

    QString selectedNote;
    QSet<QString> multi; // note ids picked with Ctrl/Shift; empty = just the open note
    QString dropKey;
    int hoverRow = -1;
    // Motion: rows that just appeared fade in; a toggled arrow turns.
    QSet<QString> fresh;
    QString turnKey;
    bool turnOpen = false;
    qreal motion = 1.0;
    static QRect headingButton(const QRect &row, int i); // 0 sort, 1 new notebook, 2 new note
};

class ShelfView : public QListView {
    Q_OBJECT
public:
    explicit ShelfView(QWidget *parent = nullptr);
    ShelfDelegate *delegate() const { return m_delegate; }
    QStringList selectedNotes() const; // multi selection in row order
    void clearMulti();

signals:
    void activated(int row, bool viaKeyboard);
    void toggle(int row);
    void contextMenu(int row, const QPoint &global);
    void headingButton(int which, const QPoint &global);
    void dropped(const QString &mime, const QString &id, int row);
    void deletePressed(int row);
    void renamePressed(int row);

protected:
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
    void dragLeaveEvent(QDragLeaveEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void startDrag(Qt::DropActions actions) override;

private:
    ShelfModel *shelfModel() const;
    int dropRowAt(const QPoint &pos, const QMimeData *mime) const;
    void toggleMulti(const ShelfRow &r);
    void selectRange(int from, int to);

    ShelfDelegate *m_delegate;
    QTimer m_expandTimer;
    int m_expandRow = -1;
    QString m_anchorKey;     // where a Shift range starts
    int m_releaseOpens = -1; // a plain press inside the selection opens on release unless it drags
};

class Shelf : public QWidget {
    Q_OBJECT
public:
    explicit Shelf(QWidget *parent = nullptr);

    void setSession(Session *session);
    void setCurrentNote(const QString &noteId); // selects and reveals it
    void rebuild();
    void focusTree();

signals:
    void openNote(const QString &id);
    void newNote(const QString &folderId);
    void moveNotes(const QStringList &ids);
    void searchRequested();
    void vaultMenuRequested(const QPoint &global);
    void emptyTrashRequested();
    void diffRequested(const QString &noteId, const QString &otherId = QString());

private:
    void onActivated(int row, bool keyboard);
    void onToggle(int row);
    void onContextMenu(int row, const QPoint &global);
    void onHeadingButton(int which, const QPoint &global);
    void onDropped(const QString &mime, const QString &ids, int row);
    void deleteRow(int row);
    void trashNotes(const QStringList &ids, bool trashed);
    void batchMenu(const QStringList &ids, bool trash, const QPoint &global);
    void renameRow(int row);
    void newFolder(const QString &parentId);
    void updateHeader();
    void saveState();
    void loadState();

    QPointer<Session> m_session;
    SyncEngine *m_engine = nullptr;
    ShelfModel *m_model;
    ShelfView *m_view;
    QToolButton *m_vaultButton;
    IconButton *m_searchButton, *m_newButton;
    QSet<QString> m_expanded;
    QHash<QString, int> m_limits;
    SortBy m_sort = SortBy::Updated;
    QString m_current;
    QTimer m_rebuildTimer;
    QVariantAnimation m_motion;
};

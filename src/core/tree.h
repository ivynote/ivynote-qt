// The shelf is one tree flattened into rows: pinned, notebooks with their
// notes, root notes, tags and the trash (guide 11.2, web tree.ts).
#pragma once

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

struct Note;
struct Folder;
struct Tag;

enum class SortBy { Updated, Created, Title };

struct TreeRow {
    enum Kind { Group, Heading, Create, FolderRow, TagRow, NoteRow, More };
    QString key;    // unique; folders and groups use it as their expansion key
    Kind kind = NoteRow;
    int depth = 0;
    QString parent; // key of the enclosing branch
    QString label;  // group/heading: an i18n source string
    QString icon;
    int count = 0;
    bool expandable = false;
    bool open = false;
    bool gap = false;
    bool trash = false;       // a note row inside the trash
    QString dropFolder;       // notebook a drop files into; "\x01" = not a target
    const Folder *folder = nullptr;
    const Tag *tag = nullptr;
    const Note *note = nullptr;
};

inline const QString kNoDrop = QStringLiteral("\x01");
constexpr int kTreePage = 100;

struct TreeInput {
    QList<const Folder *> folders;
    QList<const Note *> notes;
    QList<const Tag *> tags;
    QSet<QString> expanded;
    SortBy sortBy = SortBy::Updated;
    QHash<QString, int> limits; // branch key -> notes shown
};

QList<const Note *> sortNotes(QList<const Note *> list, SortBy by, bool trash = false);
QList<const Folder *> sortFolders(QList<const Folder *> list);
QHash<QString, int> folderTotals(const QList<const Folder *> &folders, const QList<const Note *> &notes);
QList<TreeRow> buildTree(const TreeInput &in);

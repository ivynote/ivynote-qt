#include "core/tree.h"

#include "core/models.h"

#include <QCollator>
#include <QLocale>
#include <algorithm>
#include <functional>

namespace {

const QCollator &collator() {
    static const QCollator c = [] {
        QCollator x{QLocale(QLocale::Chinese, QLocale::China)};
        x.setNumericMode(true);
        x.setCaseSensitivity(Qt::CaseInsensitive);
        return x;
    }();
    return c;
}

} // namespace

QList<const Note *> sortNotes(QList<const Note *> list, SortBy by, bool trash) {
    std::stable_sort(list.begin(), list.end(), [&](const Note *a, const Note *b) {
        if (!trash && a->pinned != b->pinned) return a->pinned;
        if (by == SortBy::Title) {
            // Untitled notes go last.
            if (a->title.isEmpty() != b->title.isEmpty()) return b->title.isEmpty();
            const int c = collator().compare(a->title, b->title);
            if (c != 0) return c < 0;
            return a->updatedAt > b->updatedAt;
        }
        if (by == SortBy::Created) return a->createdAt > b->createdAt;
        if (trash && a->trashedAt != b->trashedAt) return a->trashedAt > b->trashedAt;
        return a->updatedAt > b->updatedAt;
    });
    return list;
}

QList<const Folder *> sortFolders(QList<const Folder *> list) {
    std::stable_sort(list.begin(), list.end(), [](const Folder *a, const Folder *b) {
        if (a->sort != b->sort) return a->sort < b->sort;
        return collator().compare(a->name, b->name) < 0;
    });
    return list;
}

QHash<QString, int> folderTotals(const QList<const Folder *> &folders, const QList<const Note *> &notes) {
    QHash<QString, const Folder *> map;
    for (const Folder *f : folders) map.insert(f->id, f);
    QHash<QString, int> direct;
    for (const Note *n : notes)
        if (!n->trashed()) direct[n->folderId]++;
    QHash<QString, int> out;
    for (const Folder *f : folders) {
        const int own = direct.value(f->id);
        const Folder *p = f;
        for (int d = 0; p && d < 64; ++d) {
            out[p->id] += own;
            p = map.value(p->parentId);
        }
    }
    return out;
}

QList<TreeRow> buildTree(const TreeInput &in) {
    QList<TreeRow> rows;
    QList<const Note *> live, trashed;
    for (const Note *n : in.notes) (n->trashed() ? trashed : live) << n;
    QSet<QString> ids;
    for (const Folder *f : in.folders) ids.insert(f->id);

    QHash<QString, QList<const Folder *>> kids;
    for (const Folder *f : sortFolders(in.folders)) {
        const QString key = !f->parentId.isEmpty() && ids.contains(f->parentId) ? f->parentId : QString();
        kids[key] << f;
    }
    // A note whose notebook is gone must still be somewhere: it falls to the root.
    QHash<QString, QList<const Note *>> filed;
    for (const Note *n : live) filed[ids.contains(n->folderId) ? n->folderId : QString()] << n;
    const QHash<QString, int> totals = folderTotals(in.folders, in.notes);

    auto noteRows = [&](const QList<const Note *> &list, const QString &parent, int depth, bool trash, const QString &drop) {
        const auto sorted = sortNotes(list, in.sortBy, trash);
        const int shown = in.limits.value(parent, kTreePage);
        for (int i = 0; i < sorted.size() && i < shown; ++i) {
            TreeRow r;
            r.key = parent + u'/' + sorted[i]->id;
            r.kind = TreeRow::NoteRow;
            r.depth = depth;
            r.parent = parent;
            r.label = sorted[i]->title;
            r.icon = QStringLiteral("file");
            r.trash = trash;
            r.dropFolder = drop;
            r.note = sorted[i];
            rows << r;
        }
        if (sorted.size() > shown) {
            TreeRow r;
            r.key = parent + QStringLiteral("/more");
            r.kind = TreeRow::More;
            r.depth = depth;
            r.parent = parent;
            r.count = int(sorted.size()) - shown;
            r.dropFolder = kNoDrop;
            rows << r;
        }
    };

    auto group = [&](const QString &key, const QString &label, const QString &icon, const QList<const Note *> &list, bool gap, bool trash) {
        TreeRow r;
        r.key = key;
        r.kind = TreeRow::Group;
        r.label = label;
        r.icon = icon;
        r.count = int(list.size());
        r.expandable = !list.isEmpty();
        r.open = in.expanded.contains(key) && !list.isEmpty();
        r.gap = gap;
        r.dropFolder = kNoDrop;
        rows << r;
        if (r.open) noteRows(list, key, 1, trash, kNoDrop);
    };

    std::function<void(const QString &, int)> folderRows = [&](const QString &parent, int depth) {
        for (const Folder *f : kids.value(parent)) {
            const auto subs = kids.value(f->id);
            const auto own = filed.value(f->id);
            TreeRow r;
            r.key = f->id;
            r.kind = TreeRow::FolderRow;
            r.depth = depth;
            r.parent = parent;
            r.label = f->name;
            r.icon = QStringLiteral("folder");
            r.count = totals.value(f->id);
            r.expandable = !subs.isEmpty() || !own.isEmpty();
            r.open = r.expandable && in.expanded.contains(f->id);
            r.dropFolder = f->id;
            r.folder = f;
            rows << r;
            if (!r.open) continue;
            if (depth < 64) folderRows(f->id, depth + 1);
            noteRows(own, f->id, depth + 1, false, f->id);
        }
    };

    QList<const Note *> pinned;
    for (const Note *n : live)
        if (n->pinned) pinned << n;
    if (!pinned.isEmpty()) group(QStringLiteral("v:pinned"), QStringLiteral("置顶"), QStringLiteral("pin"), pinned, false, false);

    TreeRow heading;
    heading.key = QStringLiteral("h:notes");
    heading.kind = TreeRow::Heading;
    heading.label = QStringLiteral("笔记");
    heading.gap = !pinned.isEmpty();
    heading.dropFolder = QString();
    rows << heading;
    if (!in.folders.isEmpty()) {
        folderRows(QString(), 0);
    } else {
        TreeRow c;
        c.key = QStringLiteral("h:create");
        c.kind = TreeRow::Create;
        c.label = QStringLiteral("新建笔记本");
        c.icon = QStringLiteral("plus");
        c.dropFolder = kNoDrop;
        rows << c;
    }
    noteRows(filed.value(QString()), QString(), 0, false, QString());

    if (!in.tags.isEmpty()) {
        TreeRow g;
        g.key = QStringLiteral("v:tags");
        g.kind = TreeRow::Group;
        g.label = QStringLiteral("标签");
        g.icon = QStringLiteral("tag");
        g.expandable = true;
        g.open = in.expanded.contains(g.key);
        g.gap = true;
        g.count = int(in.tags.size());
        g.dropFolder = kNoDrop;
        rows << g;
        if (g.open) {
            QList<const Tag *> tags = in.tags;
            std::stable_sort(tags.begin(), tags.end(), [](const Tag *a, const Tag *b) { return collator().compare(a->name, b->name) < 0; });
            for (const Tag *t : tags) {
                QList<const Note *> list;
                for (const Note *n : live)
                    if (n->tags.contains(t->id)) list << n;
                TreeRow r;
                r.key = QStringLiteral("t:") + t->id;
                r.kind = TreeRow::TagRow;
                r.depth = 1;
                r.parent = g.key;
                r.label = t->name;
                r.icon = QStringLiteral("hash");
                r.count = int(list.size());
                r.expandable = !list.isEmpty();
                r.open = r.expandable && in.expanded.contains(r.key);
                r.dropFolder = kNoDrop;
                r.tag = t;
                rows << r;
                if (r.open) noteRows(list, r.key, 2, false, kNoDrop);
            }
        }
    }
    group(QStringLiteral("v:trash"), QStringLiteral("回收站"), QStringLiteral("trash"), trashed, in.tags.isEmpty(), true);
    return rows;
}

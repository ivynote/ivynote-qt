#include "core/agenda.h"

#include "core/notelock.h"

#include <QMap>
#include <algorithm>

QList<AgendaTask> agendaTasks(const QHash<QString, Note> &notes, bool includeDoneToday, const QDateTime &now) {
    const QDate today = now.date();
    QList<AgendaTask> result;
    for (const Note &note : notes) {
        if (note.deleted || note.trashed() || !note.body.contains(QStringLiteral("📅")) || notelock::isEncrypted(note.body))
            continue;
        for (const tasks::TaskLine &t : tasks::parseTasks(note.body)) {
            if (!t.due || !t.due->valid()) continue;
            if (t.done && (!includeDoneToday || t.doneDate != today)) continue;
            AgendaTask item;
            item.noteId = note.id;
            item.title = note.title;
            item.text = t.text;
            item.allDay = t.due->allDay();
            item.due = QDateTime(t.due->date, item.allDay ? QTime(9, 0) : t.due->time);
            item.line = t.line;
            item.done = t.done;
            item.doneDate = t.doneDate;
            item.repeat = t.repeat;
            item.rawLine = t.raw;
            item.daysFromToday = int(today.daysTo(t.due->date));
            result.append(item);
        }
    }
    auto sortKey = [](const AgendaTask &a) {
        // Open tasks before done-today; within a day, timed tasks before all-day tasks.
        const QDateTime effectiveDue = a.allDay ? QDateTime(a.due.date(), QTime(23, 59, 59, 999)) : a.due;
        return std::tuple(a.done ? 1 : 0, effectiveDue, a.title, a.noteId, a.line);
    };
    std::stable_sort(result.begin(), result.end(), [&](const AgendaTask &a, const AgendaTask &b) {
        return sortKey(a) < sortKey(b);
    });
    return result;
}

AgendaTree buildAgendaTree(const QHash<QString, Note> &notes, bool includeDoneToday, const QDateTime &now, bool zh) {
    AgendaTree tree;
    const QDate today = now.date();
    for (const Note &note : notes) {
        if (!note.deleted && !note.trashed() && notelock::isEncrypted(note.body)) ++tree.encryptedCount;
    }

    const QList<AgendaTask> items = agendaTasks(notes, includeDoneToday, now);
    QHash<QString, int> branchIndex;

    auto ensureBranch = [&](const QString &key, AgendaBranch::Kind kind, const QString &sortDate, const QString &label) -> AgendaBranch & {
        auto it = branchIndex.constFind(key);
        if (it != branchIndex.constEnd()) return tree.branches[*it];
        branchIndex.insert(key, int(tree.branches.size()));
        tree.branches.append(AgendaBranch{key, kind, sortDate, label, {}, 0});
        return tree.branches.last();
    };

    const QString todayIso = today.toString(Qt::ISODate);
    for (const AgendaTask &it : items) {
        const QDate day = it.due.date();
        const int delta = int(today.daysTo(day));
        const bool isOverdue = !it.done && (it.allDay ? delta < 0 : it.due < now);
        if (!it.done) {
            ++tree.totalOpen;
            if (isOverdue) ++tree.overdueCount;
            else if (delta == 0) ++tree.todayCount;
        }

        if (it.done || (delta == 0 && !isOverdue)) {
            auto &b = ensureBranch(QStringLiteral("d:") + todayIso, AgendaBranch::Today, todayIso, tasks::describeDay(today, today, zh));
            b.items.append(it);
            if (!it.done) ++b.openCount;
        } else if (isOverdue) {
            auto &b = ensureBranch(QStringLiteral("overdue"), AgendaBranch::Overdue, QString(), zh ? QStringLiteral("已逾期") : QStringLiteral("Overdue"));
            b.items.append(it);
            ++b.openCount;
        } else if (delta == 1) {
            const QString iso = day.toString(Qt::ISODate);
            auto &b = ensureBranch(QStringLiteral("d:") + iso, AgendaBranch::Tomorrow, iso, tasks::describeDay(day, today, zh));
            b.items.append(it);
            ++b.openCount;
        } else if (delta >= 2 && delta < 7) {
            const QString iso = day.toString(Qt::ISODate);
            auto &b = ensureBranch(QStringLiteral("d:") + iso, AgendaBranch::Day, iso, tasks::describeDay(day, today, zh));
            b.items.append(it);
            ++b.openCount;
        } else {
            const QString ym = day.toString(QStringLiteral("yyyy-MM"));
            auto &b = ensureBranch(QStringLiteral("m:") + ym, AgendaBranch::Month, ym, tasks::describeMonth(day.year(), day.month(), today, zh));
            b.items.append(it);
            ++b.openCount;
        }
    }

    auto rank = [](const AgendaBranch &b) -> QString {
        if (b.kind == AgendaBranch::Overdue) return QStringLiteral("0");
        return QStringLiteral("1") + b.sortDate + (b.kind == AgendaBranch::Month ? QStringLiteral("-99") : QString());
    };
    std::stable_sort(tree.branches.begin(), tree.branches.end(), [&](const AgendaBranch &a, const AgendaBranch &b) {
        return rank(a) < rank(b);
    });
    return tree;
}

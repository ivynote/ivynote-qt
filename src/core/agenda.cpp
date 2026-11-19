#include "core/agenda.h"
#include "core/notelock.h"
#include <QRegularExpression>
#include <algorithm>

QList<AgendaTask> agendaTasks(const QHash<QString, Note> &notes) {
    static const QRegularExpression task(QStringLiteral(R"(^\s*(?:[-*+]|\d+[.)])\s+\[ \]\s*(.*)$)"));
    static const QRegularExpression date(QStringLiteral(R"(📅️?\s*(\d{4}-\d\d-\d\d)(?:[ T](\d{1,2}:\d\d))?)"));
    QList<AgendaTask> result;
    for (const Note &note : notes) {
        if (note.deleted || note.trashed() || !note.body.contains(QStringLiteral("📅")) || notelock::isEncrypted(note.body)) continue;
        const auto lines = note.body.split(u'\n');
        QString fence;
        for (int line = 0; line < lines.size(); ++line) {
            const auto trimmed = lines[line].trimmed();
            if (!fence.isEmpty()) {
                if (trimmed.startsWith(fence) && trimmed == QString(trimmed.size(), fence.front())) fence.clear();
                continue;
            }
            if (trimmed.startsWith(QStringLiteral("```")) || trimmed.startsWith(QStringLiteral("~~~"))) {
                int count = 3;
                while (count < trimmed.size() && trimmed[count] == trimmed.front()) ++count;
                fence = trimmed.left(count);
                continue;
            }
            const auto match = task.match(lines[line]);
            if (!match.hasMatch()) continue;
            QString text = match.captured(1);
            const auto marker = date.match(text);
            if (!marker.hasMatch()) continue;
            const auto day = QDate::fromString(marker.captured(1), Qt::ISODate);
            const auto time = QTime::fromString(marker.captured(2), QStringLiteral("H:mm"));
            if (!day.isValid()) continue;
            text.remove(marker.capturedStart(), marker.capturedLength());
            result.append({note.id, note.title, text.trimmed(), QDateTime(day, time.isValid() ? time : QTime(9, 0)), line, !time.isValid()});
        }
    }
    std::stable_sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
        return std::tie(a.due, a.title, a.noteId, a.line) < std::tie(b.due, b.title, b.noteId, b.line);
    });
    return result;
}

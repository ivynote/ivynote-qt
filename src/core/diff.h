// Line and intraline diff plus conflict-copy matching, matching the web/Tauri client.
#pragma once

#include "core/models.h"

#include <QHash>
#include <QList>
#include <QString>
#include <optional>

namespace diff {

enum class Kind { Same, Del, Add };

struct Span {
    Kind kind = Kind::Same;
    QString text;
};

struct Line {
    Kind kind = Kind::Same;
    int oldLine = 0; // 1-based, 0 when absent
    int newLine = 0; // 1-based, 0 when absent
    QString text;
    QList<Span> spans; // populated for paired del/add lines
};

struct Row {
    std::optional<Line> left;
    std::optional<Line> right;
};

struct Result {
    QList<Row> rows;
    QList<Line> unified;
    int added = 0;
    int removed = 0;
    int unchanged = 0;
    bool identical = false;
};

// Detects "(冲突副本 …)" / "（冲突副本…）" / "(conflict copy …)" suffixes.
bool isConflictNote(const QString &title);
QString conflictBaseTitle(const QString &title); // empty when not a conflict copy
QList<const Note *> findConflictCounterparts(const Note &current, const QHash<QString, Note> &all);

// Computes LCS line diff with intraline highlight spans on paired edits.
Result compute(const QString &oldText, const QString &newText);

} // namespace diff

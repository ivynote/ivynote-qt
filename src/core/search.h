// Instant local search (works offline, CJK friendly), as in the web client.
#pragma once

#include <QList>
#include <QPair>
#include <QString>

struct Note;

struct SearchHit {
    const Note *note = nullptr;
    double score = 0;
    QString snippet;
    QList<QPair<int, int>> marks; // [start, end) inside snippet
};

QList<SearchHit> searchNotes(const QList<const Note *> &notes, const QString &query, qint64 now, int limit = 50);

#pragma once
#include "core/models.h"
#include <QDateTime>

struct AgendaTask {
    QString noteId, title, text;
    QDateTime due;
    int line;
    bool allDay;
};

QList<AgendaTask> agendaTasks(const QHash<QString, Note> &notes);

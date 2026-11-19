#pragma once

#include "core/models.h"
#include "core/tasks.h"

#include <QDateTime>
#include <QList>
#include <optional>

struct AgendaTask {
    QString noteId, title, text;
    QDateTime due;
    int line = 0;
    bool allDay = true;
    bool done = false;
    QDate doneDate;
    std::optional<tasks::RepeatRule> repeat;
    QString rawLine;
    int daysFromToday = 0;
};

struct AgendaBranch {
    enum Kind { Overdue, Today, Tomorrow, Day, Month };
    QString key;
    Kind kind = Today;
    QString sortDate; // YYYY-MM-DD or YYYY-MM
    QString label;
    QList<AgendaTask> items;
    int openCount = 0;

    bool openByDefault() const { return kind == Overdue || kind == Today || kind == Tomorrow; }
};

struct AgendaTree {
    QList<AgendaBranch> branches;
    int totalOpen = 0;
    int todayCount = 0;
    int overdueCount = 0;
    int encryptedCount = 0;
};

QList<AgendaTask> agendaTasks(const QHash<QString, Note> &notes, bool includeDoneToday = false,
                              const QDateTime &now = QDateTime::currentDateTime());

AgendaTree buildAgendaTree(const QHash<QString, Note> &notes, bool includeDoneToday,
                           const QDateTime &now, bool zh);

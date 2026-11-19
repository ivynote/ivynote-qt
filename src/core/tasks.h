// Obsidian Tasks-compatible task syntax, recurrence, toggling, and natural-language
// date parsing (@ completion), aligned with web/Tauri's tasks.ts and when.ts.
#pragma once

#include <QDate>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QTime>
#include <optional>

namespace tasks {

struct Due {
    QDate date;
    QTime time; // invalid when all-day

    bool valid() const { return date.isValid(); }
    bool allDay() const { return !time.isValid(); }
    QString dateString() const { return date.isValid() ? date.toString(Qt::ISODate) : QString(); }
    QString timeString() const { return time.isValid() ? time.toString(QStringLiteral("HH:mm")) : QString(); }
    QString formatMarker() const; // "📅 YYYY-MM-DD[ HH:mm]"
};

enum class RepeatUnit { Day, Week, Month, Year, Weekday };

struct RepeatRule {
    int every = 1;
    RepeatUnit unit = RepeatUnit::Day;
    QString raw; // canonical English form, e.g. "every day", "every 2 weeks", "every weekday"
};

struct TaskLine {
    int line = 0; // 0-based
    bool done = false;
    QString raw;
    QString text; // display text with markers stripped
    std::optional<Due> due;
    std::optional<RepeatRule> repeat;
    QDate doneDate;
};

struct QuickDate {
    QString label;
    Due due;
};

struct RepeatChoice {
    QString raw;   // "" for never, or canonical "every ..."
    QString label; // localized display label
};

// Parse / format recurrence rules.
std::optional<RepeatRule> parseRepeat(const QString &s);
QString describeRepeat(const RepeatRule &r, bool zh);
QList<RepeatChoice> repeatChoices(bool zh);
QDate nextDate(const QDate &date, const RepeatRule &rule);

// Parse task lines (code fences skipped in parseTasks).
bool isTaskLine(const QString &line);
std::optional<TaskLine> parseTaskLine(const QString &line, int lineNo = 0);
QList<TaskLine> parseTasks(const QString &body);

// Line mutations.
// Converts a plain or bullet/numbered line into "- [ ] ..." preserving indentation.
QString lineAsTask(const QString &line);
// Sets or removes due date (and optionally repeat rule; pass std::nullopt for repeat to keep existing,
// or empty string "" to clear repeat).
QString setTaskDue(const QString &line, const std::optional<Due> &due, const std::optional<QString> &repeat = std::nullopt);
// Toggles task at 0-based lineNo:
// - Plain task: flips [ ] <-> [x] only
// - Dated/repeating task: appends "✅ YYYY-MM-DD" on check; removes "✅ ..." on uncheck
// - Repeating task with due: inserts next unchecked occurrence on the line immediately above
QString toggleTaskLine(const QString &body, int lineNo, const QDate &today = QDate::currentDate());

// Natural-language date/time parsing (@明天 9点, @周五下午3点, @10分钟后, @next monday, etc.).
std::optional<Due> parseWhen(const QString &input, const QDateTime &now = QDateTime::currentDateTime());
QList<QuickDate> quickDates(const QDateTime &now, bool zh);

// Localized human descriptions for chips and agenda headers.
QString describeDue(const Due &due, const QDate &today, bool zh);
QString describeDay(const QDate &date, const QDate &today, bool zh);
QString describeMonth(int year, int month, const QDate &today, bool zh);

} // namespace tasks

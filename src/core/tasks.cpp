#include "core/tasks.h"

#include "core/markdown.h"

#include <QLocale>
#include <QRegularExpression>
#include <cmath>

namespace tasks {

namespace {

const QRegularExpression &taskRe() {
    static const QRegularExpression re(QStringLiteral(R"(^((?:\s*>\s*)*\s*(?:[-*+]|\d+[.)])\s+\[)([ xX])(\]\s?)(.*)$)"));
    return re;
}

const QRegularExpression &dueRe() {
    static const QRegularExpression re(QStringLiteral(R"(📅️?\s*(\d{4}-\d{2}-\d{2})(?:[ T](\d{1,2}:\d{2}))?)"));
    return re;
}

const QRegularExpression &doneRe() {
    static const QRegularExpression re(QStringLiteral(R"(✅️?\s*(\d{4}-\d{2}-\d{2}))"));
    return re;
}

const QRegularExpression &repeatRe() {
    static const QRegularExpression re(QStringLiteral(R"(🔁️?\s*([^📅✅⏳🛫➕⏫🔼🔽🔺⏬\n]*))"));
    return re;
}

QDate addMonthsClamped(const QDate &d, int months) {
    // QDate::addMonths already clamps Jan 31 + 1 month -> Feb 28/29 and Feb 29 + 12 months -> Feb 28.
    return d.addMonths(months);
}

QTime parseStrictHm(const QString &s) {
    if (s.isEmpty()) return {};
    const auto parts = s.split(u':');
    if (parts.size() != 2) return {};
    bool okH = false, okM = false;
    const int h = parts[0].toInt(&okH);
    const int m = parts[1].toInt(&okM);
    if (!okH || !okM || h < 0 || h > 23 || m < 0 || m > 59) return {};
    return QTime(h, m);
}

// Parse small positive/non-negative Arabic or Chinese numerals (0..9999 / 零..九十九).
int parseNum(const QString &s) {
    bool ok = false;
    const int arabic = s.toInt(&ok);
    if (ok) return arabic;
    static const QHash<QChar, int> digits{
        {u'零', 0}, {u'〇', 0}, {u'一', 1}, {u'二', 2}, {u'两', 2}, {u'兩', 2},
        {u'三', 3}, {u'四', 4}, {u'五', 5}, {u'六', 6}, {u'七', 7}, {u'八', 8}, {u'九', 9},
    };
    if (s == QStringLiteral("十")) return 10;
    const qsizetype ten = s.indexOf(u'十');
    if (ten >= 0) {
        int tens = 1;
        if (ten > 0) {
            if (!digits.contains(s[ten - 1])) return -1;
            tens = digits.value(s[ten - 1]);
        }
        int ones = 0;
        if (ten + 1 < s.size()) {
            if (!digits.contains(s[ten + 1])) return -1;
            ones = digits.value(s[ten + 1]);
        }
        return tens * 10 + ones;
    }
    if (s.size() == 1 && digits.contains(s[0])) return digits.value(s[0]);
    return -1;
}

int parseZhWeekday(QChar ch) {
    // Returns Qt dayOfWeek (1 = Mon .. 7 = Sun), or 0 on failure.
    switch (ch.unicode()) {
    case u'一': return 1;
    case u'二': return 2;
    case u'三': return 3;
    case u'四': return 4;
    case u'五': return 5;
    case u'六': return 6;
    case u'日':
    case u'天': return 7;
    default: return 0;
    }
}

int parseEnWeekday(const QString &s) {
    const QString k = s.left(3).toLower();
    if (k == QLatin1String("mon")) return 1;
    if (k == QLatin1String("tue")) return 2;
    if (k == QLatin1String("wed")) return 3;
    if (k == QLatin1String("thu")) return 4;
    if (k == QLatin1String("fri")) return 5;
    if (k == QLatin1String("sat")) return 6;
    if (k == QLatin1String("sun")) return 7;
    return 0;
}

QString zhWeekdayName(int qtDayOfWeek) {
    static const char *names[] = {"周一", "周二", "周三", "周四", "周五", "周六", "周日"};
    if (qtDayOfWeek < 1 || qtDayOfWeek > 7) return {};
    return QString::fromUtf8(names[qtDayOfWeek - 1]);
}

QString enWeekdayShort(int qtDayOfWeek) {
    static const char *names[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    if (qtDayOfWeek < 1 || qtDayOfWeek > 7) return {};
    return QString::fromLatin1(names[qtDayOfWeek - 1]);
}

QString enMonthShort(int month) {
    static const char *names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (month < 1 || month > 12) return {};
    return QString::fromLatin1(names[month - 1]);
}

QString enMonthFull(int month) {
    static const char *names[] = {"January", "February", "March", "April", "May", "June",
                                  "July", "August", "September", "October", "November", "December"};
    if (month < 1 || month > 12) return {};
    return QString::fromLatin1(names[month - 1]);
}

struct TimeResult {
    QTime time;
    QString rest;
};

TimeResult takeTime(QString s) {
    // Period words and their default hours.
    struct Period {
        const char *word;
        int defHour;
        bool pmBias;
        bool midnightBias;
        const char *dayRewrite;
    };
    static const Period periods[] = {
        {"今晚", 20, true, false, "今天"},
        {"明晚", 20, true, false, "明天"},
        {"凌晨", 6, false, true, nullptr},
        {"早上", 8, false, false, nullptr},
        {"早晨", 8, false, false, nullptr},
        {"上午", 9, false, false, nullptr},
        {"中午", 12, false, false, nullptr},
        {"下午", 15, true, false, nullptr},
        {"傍晚", 18, true, false, nullptr},
        {"晚上", 20, true, false, nullptr},
        {"夜里", 22, true, false, nullptr},
    };
    int defHour = -1;
    bool pmBias = false;
    bool midnightBias = false;
    for (const auto &p : periods) {
        const QString w = QString::fromUtf8(p.word);
        const qsizetype idx = s.indexOf(w);
        if (idx < 0) continue;
        defHour = p.defHour;
        pmBias = p.pmBias;
        midnightBias = p.midnightBias;
        if (p.dayRewrite) s.replace(idx, w.size(), QString::fromUtf8(p.dayRewrite));
        else s.remove(idx, w.size());
        break;
    }

    auto adjustHour = [&](int h, bool hasExplicitAmPm) -> int {
        if (!hasExplicitAmPm) {
            if (pmBias && h >= 0 && h < 12) h += 12;
            else if (midnightBias && h == 12) h = 0;
        }
        return h;
    };

    // 1. 12-hour AM/PM
    static const QRegularExpression ampm(QStringLiteral(R"(\b(\d{1,2})(?::(\d{2}))?\s*(am|pm)\b)"),
                                         QRegularExpression::CaseInsensitiveOption);
    if (auto m = ampm.match(s); m.hasMatch()) {
        int h = m.captured(1).toInt();
        const int min = m.captured(2).isEmpty() ? 0 : m.captured(2).toInt();
        const bool pm = m.captured(3).compare(QLatin1String("pm"), Qt::CaseInsensitive) == 0;
        if (h >= 1 && h <= 12 && min >= 0 && min <= 59) {
            if (pm && h < 12) h += 12;
            if (!pm && h == 12) h = 0;
            s.remove(m.capturedStart(), m.capturedLength());
            return {QTime(h, min), s};
        }
    }

    // 2. Colon HH:mm (including fullwidth colon)
    static const QRegularExpression colon(QStringLiteral(R"((\d{1,2})[:：](\d{2}))"));
    if (auto m = colon.match(s); m.hasMatch()) {
        int h = adjustHour(m.captured(1).toInt(), false);
        const int min = m.captured(2).toInt();
        if (h >= 0 && h <= 23 && min >= 0 && min <= 59) {
            s.remove(m.capturedStart(), m.capturedLength());
            return {QTime(h, min), s};
        }
    }

    // 3. Chinese clock: N点(半|一刻|三刻|N分?)
    static const QRegularExpression zhClock(
        QStringLiteral(R"((\d{1,2}|[零〇一二两三四五六七八九十]{1,3})\s*[点點时](半|一刻|三刻|(?:(\d{1,2}|[零〇一二两三四五六七八九十]{1,3})分?))?)"));
    if (auto m = zhClock.match(s); m.hasMatch()) {
        const int rawH = parseNum(m.captured(1));
        if (rawH >= 0 && rawH <= 23) {
            int min = 0;
            const QString tail = m.captured(2);
            bool validMin = true;
            if (tail == QStringLiteral("半")) min = 30;
            else if (tail == QStringLiteral("一刻")) min = 15;
            else if (tail == QStringLiteral("三刻")) min = 45;
            else if (!m.captured(3).isEmpty()) {
                min = parseNum(m.captured(3));
                validMin = min >= 0 && min <= 59;
            }
            if (validMin) {
                const int h = adjustHour(rawH, false);
                if (h >= 0 && h <= 23) {
                    s.remove(m.capturedStart(), m.capturedLength());
                    return {QTime(h, min), s};
                }
            }
        }
    }

    // 4. Period word alone
    if (defHour >= 0) return {QTime(defHour, 0), s};
    return {{}, s};
}

struct DateResult {
    QDate date;
    QString rest;
};

DateResult takeDate(QString s, const QDate &today) {
    const QString lower = s.toLower();

    // 1. Relative named days
    static const QRegularExpression relDays(
        QStringLiteral(R"(大后天|后天|day\s+after\s+tomorrow|明天|明日|tomorrow|tmr|今天|今日|today|tonight|昨天|yesterday)"),
        QRegularExpression::CaseInsensitiveOption);
    if (auto m = relDays.match(lower); m.hasMatch()) {
        const QString tok = m.captured(0);
        int delta = 0;
        if (tok == QStringLiteral("大后天")) delta = 3;
        else if (tok == QStringLiteral("后天") || tok.startsWith(QLatin1String("day"))) delta = 2;
        else if (tok == QStringLiteral("明天") || tok == QStringLiteral("明日") || tok == QLatin1String("tomorrow") || tok == QLatin1String("tmr")) delta = 1;
        else if (tok == QStringLiteral("昨天") || tok == QLatin1String("yesterday")) delta = -1;
        s.remove(m.capturedStart(), m.capturedLength());
        return {today.addDays(delta), s};
    }

    // 2. Full date YYYY-M-D / YYYY年M月D日
    static const QRegularExpression fullDate(QStringLiteral(R"((\d{4})[-/.年](\d{1,2})[-/.月](\d{1,2})[日号]?)"));
    if (auto m = fullDate.match(s); m.hasMatch()) {
        const QDate d(m.captured(1).toInt(), m.captured(2).toInt(), m.captured(3).toInt());
        if (d.isValid()) {
            s.remove(m.capturedStart(), m.capturedLength());
            return {d, s};
        }
    }

    // 3a. Month-day numeric: M-D / M/D / M.D / M月D日
    static const QRegularExpression mdNum(QStringLiteral(R"((?:^|[^\d])(\d{1,2})[-/.月](\d{1,2})[日号]?)"));
    if (auto m = mdNum.match(s); m.hasMatch()) {
        const int mo = m.captured(1).toInt();
        const int da = m.captured(2).toInt();
        QDate d(today.year(), mo, da);
        if (d.isValid()) {
            if (d < today) d = QDate(today.year() + 1, mo, da);
            if (d.isValid()) {
                s.remove(m.capturedStart(1), m.capturedEnd() - m.capturedStart(1));
                return {d, s};
            }
        }
    }

    // 3b. Month-day Chinese numerals: 十一月二十日
    static const QRegularExpression mdZh(
        QStringLiteral(R"((\d{1,2}|[零〇一二两三四五六七八九十]{1,3})月(\d{1,2}|[零〇一二两三四五六七八九十]{1,3})[日号]?)"));
    if (auto m = mdZh.match(s); m.hasMatch()) {
        const int mo = parseNum(m.captured(1));
        const int da = parseNum(m.captured(2));
        QDate d(today.year(), mo, da);
        if (d.isValid()) {
            if (d < today) d = QDate(today.year() + 1, mo, da);
            if (d.isValid()) {
                s.remove(m.capturedStart(), m.capturedLength());
                return {d, s};
            }
        }
    }

    // 4a. Chinese relative offset: N天后 / N周后 / N个月后 / N年后
    static const QRegularExpression zhOffset(
        QStringLiteral(R"((\d{1,3}|[零〇一二两三四五六七八九十]{1,3})\s*(天|日|周|星期|礼拜|个月|月|年)[以之]?后)"));
    if (auto m = zhOffset.match(s); m.hasMatch()) {
        const int n = parseNum(m.captured(1));
        const QString u = m.captured(2);
        if (n >= 1) {
            QDate d = today;
            if (u == QStringLiteral("天") || u == QStringLiteral("日")) d = today.addDays(n);
            else if (u == QStringLiteral("周") || u == QStringLiteral("星期") || u == QStringLiteral("礼拜")) d = today.addDays(7 * n);
            else if (u == QStringLiteral("个月") || u == QStringLiteral("月")) d = addMonthsClamped(today, n);
            else if (u == QStringLiteral("年")) d = addMonthsClamped(today, 12 * n);
            s.remove(m.capturedStart(), m.capturedLength());
            return {d, s};
        }
    }

    // 4b. English relative offset: in N days/weeks/months/years
    static const QRegularExpression enOffset(
        QStringLiteral(R"(\bin\s+(\d{1,3})\s+(day|week|month|year)s?\b)"),
        QRegularExpression::CaseInsensitiveOption);
    if (auto m = enOffset.match(s); m.hasMatch()) {
        const int n = m.captured(1).toInt();
        const QString u = m.captured(2).toLower();
        if (n >= 1) {
            QDate d = today;
            if (u == QLatin1String("day")) d = today.addDays(n);
            else if (u == QLatin1String("week")) d = today.addDays(7 * n);
            else if (u == QLatin1String("month")) d = addMonthsClamped(today, n);
            else if (u == QLatin1String("year")) d = addMonthsClamped(today, 12 * n);
            s.remove(m.capturedStart(), m.capturedLength());
            return {d, s};
        }
    }

    // 5. Chinese weekday: (下下|下个?|这个?|本)?(周|星期|礼拜)([一二三四五六日天])
    static const QRegularExpression zhWd(QStringLiteral(R"((下下|下个?|这个?|本)?(?:周|星期|礼拜)([一二三四五六日天]))"));
    if (auto m = zhWd.match(s); m.hasMatch()) {
        const int target = parseZhWeekday(m.captured(2).at(0));
        if (target >= 1) {
            const QString mod = m.captured(1);
            const QDate thisMon = today.addDays(-(today.dayOfWeek() - 1));
            QDate d;
            if (mod == QStringLiteral("下下")) d = thisMon.addDays(14 + target - 1);
            else if (mod.startsWith(u'下')) d = thisMon.addDays(7 + target - 1);
            else if (mod.startsWith(u'这') || mod == QStringLiteral("本")) d = thisMon.addDays(target - 1);
            else {
                d = thisMon.addDays(target - 1);
                if (d < today) d = d.addDays(7);
            }
            s.remove(m.capturedStart(), m.capturedLength());
            return {d, s};
        }
    }

    // 6. English weekday: (next )?(sun|mon|tue|wed|thu|fri|sat)[a-z]*
    static const QRegularExpression enWd(
        QStringLiteral(R"(\b(next\s+)?(sun|mon|tue|wed|thu|fri|sat)[a-z]*\b)"),
        QRegularExpression::CaseInsensitiveOption);
    if (auto m = enWd.match(s); m.hasMatch()) {
        const int target = parseEnWeekday(m.captured(2));
        if (target >= 1) {
            const bool isNext = !m.captured(1).isEmpty();
            const QDate thisMon = today.addDays(-(today.dayOfWeek() - 1));
            QDate d;
            if (isNext) {
                d = thisMon.addDays(7 + target - 1);
            } else {
                int delta = (target - today.dayOfWeek() + 7) % 7;
                if (delta == 0) delta = 7;
                d = today.addDays(delta);
            }
            s.remove(m.capturedStart(), m.capturedLength());
            return {d, s};
        }
    }

    // 7. Named milestones: 下周 / next week / 下个月 / next month / 月底 / end of month / 周末 / weekend
    static const QRegularExpression nextWeek(QStringLiteral(R"(下个?周|next\s+week)"), QRegularExpression::CaseInsensitiveOption);
    if (auto m = nextWeek.match(s); m.hasMatch()) {
        const QDate thisMon = today.addDays(-(today.dayOfWeek() - 1));
        s.remove(m.capturedStart(), m.capturedLength());
        return {thisMon.addDays(7), s};
    }
    static const QRegularExpression nextMonth(QStringLiteral(R"(下个?月|next\s+month)"), QRegularExpression::CaseInsensitiveOption);
    if (auto m = nextMonth.match(s); m.hasMatch()) {
        const QDate firstNext = QDate(today.year(), today.month(), 1).addMonths(1);
        s.remove(m.capturedStart(), m.capturedLength());
        return {firstNext, s};
    }
    static const QRegularExpression endMonth(QStringLiteral(R"(月底|end\s+of\s+month)"), QRegularExpression::CaseInsensitiveOption);
    if (auto m = endMonth.match(s); m.hasMatch()) {
        const QDate lastDay(today.year(), today.month(), today.daysInMonth());
        s.remove(m.capturedStart(), m.capturedLength());
        return {lastDay, s};
    }
    static const QRegularExpression weekend(QStringLiteral(R"(周末|this\s+weekend|weekend)"), QRegularExpression::CaseInsensitiveOption);
    if (auto m = weekend.match(s); m.hasMatch()) {
        const int delta = (6 - today.dayOfWeek() + 7) % 7;
        s.remove(m.capturedStart(), m.capturedLength());
        return {today.addDays(delta == 0 ? 0 : delta), s};
    }

    // 8. Bare day of month: N日 / N号 (not followed by 后)
    static const QRegularExpression bareDay(
        QStringLiteral(R"((?:^|\s)(\d{1,2}|[零〇一二两三四五六七八九十]{1,3})[日号](?!后))"));
    if (auto m = bareDay.match(s); m.hasMatch()) {
        const int da = parseNum(m.captured(1));
        if (da >= 1 && da <= 31) {
            QDate d(today.year(), today.month(), da);
            if (!d.isValid() || d < today) {
                const QDate nm = QDate(today.year(), today.month(), 1).addMonths(1);
                d = QDate(nm.year(), nm.month(), da);
            }
            if (d.isValid()) {
                s.remove(m.capturedStart(1), m.capturedEnd() - m.capturedStart(1));
                return {d, s};
            }
        }
    }

    return {{}, s};
}

} // namespace

QString Due::formatMarker() const {
    if (!date.isValid()) return {};
    QString out = QStringLiteral("📅 ") + dateString();
    if (time.isValid()) out += u' ' + timeString();
    return out;
}

std::optional<RepeatRule> parseRepeat(const QString &s) {
    const QString cleaned = s.simplified().toLower();
    if (cleaned.isEmpty()) return std::nullopt;
    if (cleaned == QLatin1String("every weekday"))
        return RepeatRule{1, RepeatUnit::Weekday, QStringLiteral("every weekday")};
    static const QRegularExpression re(QStringLiteral(R"(^every\s+(?:(\d{1,3})\s+)?(day|week|month|year)s?$)"));
    const auto m = re.match(cleaned);
    if (!m.hasMatch()) return std::nullopt;
    const int every = m.captured(1).isEmpty() ? 1 : m.captured(1).toInt();
    if (every < 1) return std::nullopt;
    const QString u = m.captured(2);
    RepeatUnit unit = RepeatUnit::Day;
    if (u == QLatin1String("week")) unit = RepeatUnit::Week;
    else if (u == QLatin1String("month")) unit = RepeatUnit::Month;
    else if (u == QLatin1String("year")) unit = RepeatUnit::Year;
    const QString raw = every == 1 ? QStringLiteral("every ") + u : QStringLiteral("every %1 %2s").arg(every).arg(u);
    return RepeatRule{every, unit, raw};
}

QString describeRepeat(const RepeatRule &r, bool zh) {
    if (r.unit == RepeatUnit::Weekday) return zh ? QStringLiteral("每个工作日") : QStringLiteral("Every weekday");
    if (r.every == 1) {
        switch (r.unit) {
        case RepeatUnit::Day: return zh ? QStringLiteral("每天") : QStringLiteral("Every day");
        case RepeatUnit::Week: return zh ? QStringLiteral("每周") : QStringLiteral("Every week");
        case RepeatUnit::Month: return zh ? QStringLiteral("每月") : QStringLiteral("Every month");
        case RepeatUnit::Year: return zh ? QStringLiteral("每年") : QStringLiteral("Every year");
        case RepeatUnit::Weekday: break;
        }
    }
    if (zh) {
        switch (r.unit) {
        case RepeatUnit::Day: return QStringLiteral("每 %1 天").arg(r.every);
        case RepeatUnit::Week: return QStringLiteral("每 %1 周").arg(r.every);
        case RepeatUnit::Month: return QStringLiteral("每 %1 个月").arg(r.every);
        case RepeatUnit::Year: return QStringLiteral("每 %1 年").arg(r.every);
        case RepeatUnit::Weekday: break;
        }
    } else {
        switch (r.unit) {
        case RepeatUnit::Day: return QStringLiteral("Every %1 days").arg(r.every);
        case RepeatUnit::Week: return QStringLiteral("Every %1 weeks").arg(r.every);
        case RepeatUnit::Month: return QStringLiteral("Every %1 months").arg(r.every);
        case RepeatUnit::Year: return QStringLiteral("Every %1 years").arg(r.every);
        case RepeatUnit::Weekday: break;
        }
    }
    return r.raw;
}

QList<RepeatChoice> repeatChoices(bool zh) {
    return {
        {QString(), zh ? QStringLiteral("不重复") : QStringLiteral("Never")},
        {QStringLiteral("every day"), zh ? QStringLiteral("每天") : QStringLiteral("Every day")},
        {QStringLiteral("every weekday"), zh ? QStringLiteral("每个工作日") : QStringLiteral("Every weekday")},
        {QStringLiteral("every week"), zh ? QStringLiteral("每周") : QStringLiteral("Every week")},
        {QStringLiteral("every 2 weeks"), zh ? QStringLiteral("每 2 周") : QStringLiteral("Every 2 weeks")},
        {QStringLiteral("every month"), zh ? QStringLiteral("每月") : QStringLiteral("Every month")},
        {QStringLiteral("every year"), zh ? QStringLiteral("每年") : QStringLiteral("Every year")},
    };
}

QDate nextDate(const QDate &date, const RepeatRule &rule) {
    if (!date.isValid()) return {};
    switch (rule.unit) {
    case RepeatUnit::Day: return date.addDays(rule.every);
    case RepeatUnit::Week: return date.addDays(7 * rule.every);
    case RepeatUnit::Month: return addMonthsClamped(date, rule.every);
    case RepeatUnit::Year: return addMonthsClamped(date, 12 * rule.every);
    case RepeatUnit::Weekday: {
        QDate d = date;
        do {
            d = d.addDays(1);
        } while (d.dayOfWeek() == 6 || d.dayOfWeek() == 7);
        return d;
    }
    }
    return date;
}

bool isTaskLine(const QString &line) {
    return taskRe().match(line).hasMatch();
}

std::optional<TaskLine> parseTaskLine(const QString &line, int lineNo) {
    const auto m = taskRe().match(line);
    if (!m.hasMatch()) return std::nullopt;
    TaskLine t;
    t.line = lineNo;
    t.done = m.captured(2) != QLatin1String(" ");
    t.raw = line;
    QString rest = m.captured(4);

    if (const auto dm = dueRe().match(rest); dm.hasMatch()) {
        const QDate d = QDate::fromString(dm.captured(1), Qt::ISODate);
        if (d.isValid()) {
            t.due = Due{d, parseStrictHm(dm.captured(2))};
            rest.remove(dm.capturedStart(), dm.capturedLength());
        }
    }
    if (const auto rm = repeatRe().match(rest); rm.hasMatch()) {
        if (auto r = parseRepeat(rm.captured(1))) {
            t.repeat = *r;
            rest.remove(rm.capturedStart(), rm.capturedLength());
        }
    }
    if (const auto cm = doneRe().match(rest); cm.hasMatch()) {
        const QDate d = QDate::fromString(cm.captured(1), Qt::ISODate);
        if (d.isValid()) {
            t.doneDate = d;
            rest.remove(cm.capturedStart(), cm.capturedLength());
        }
    }
    t.text = rest.simplified();
    return t;
}

QList<TaskLine> parseTasks(const QString &body) {
    QList<TaskLine> out;
    const QStringList lines = body.split(u'\n');
    int fence = 0;
    for (int i = 0; i < lines.size(); ++i) {
        if (md::advanceFence(lines[i], fence)) continue;
        if (fence) continue;
        if (auto t = parseTaskLine(lines[i], i)) out.append(*t);
    }
    return out;
}

QString lineAsTask(const QString &line) {
    if (isTaskLine(line)) return line;
    static const QRegularExpression listPrefix(QStringLiteral(R"(^(\s*)(?:[-*+]|\d+[.)])\s+)"));
    if (auto m = listPrefix.match(line); m.hasMatch())
        return m.captured(1) + QStringLiteral("- [ ] ") + line.mid(m.capturedEnd());
    static const QRegularExpression leadWs(QStringLiteral(R"(^(\s*))"));
    const auto ws = leadWs.match(line).captured(1);
    return ws + QStringLiteral("- [ ] ") + line.mid(ws.size());
}

QString setTaskDue(const QString &line, const std::optional<Due> &due, const std::optional<QString> &repeat) {
    if (due && due->valid() && !repeat.has_value()) {
        if (const auto dm = dueRe().match(line); dm.hasMatch()) {
            QString out = line;
            out.replace(dm.capturedStart(), dm.capturedLength(), due->formatMarker());
            return out;
        }
    }

    QString out = line;
    static const QRegularExpression dueStrip(QStringLiteral(R"(\s*📅️?\s*\d{4}-\d{2}-\d{2}(?:[ T]\d{1,2}:\d{2})?)"));
    static const QRegularExpression repeatStrip(QStringLiteral(R"(\s*🔁️?\s*[^📅✅⏳🛫➕⏫🔼🔽🔺⏬\n]*)"));
    static const QRegularExpression doneTail(QStringLiteral(R"(\s*✅️?\s*\d{4}-\d{2}-\d{2})"));

    out.remove(dueStrip);
    if (repeat.has_value()) out.remove(repeatStrip);

    QString suffix;
    if (due && due->valid()) suffix += QStringLiteral(" ") + due->formatMarker();
    if (repeat.has_value() && !repeat->trimmed().isEmpty()) {
        if (auto r = parseRepeat(*repeat)) suffix += QStringLiteral(" 🔁 ") + r->raw;
    }

    // Keep ✅ at the very end if present.
    if (const auto cm = doneTail.match(out); cm.hasMatch()) {
        const QString before = out.left(cm.capturedStart()).trimmed();
        const QString donePart = out.mid(cm.capturedStart());
        // Preserve leading indentation of original line.
        int indent = 0;
        while (indent < out.size() && out[indent].isSpace()) ++indent;
        return out.left(indent) + before + suffix + donePart;
    }
    while (!out.isEmpty() && out.back() == u' ') out.chop(1);
    return out + suffix;
}

QString toggleTaskLine(const QString &body, int lineNo, const QDate &today) {
    QStringList lines = body.split(u'\n');
    if (lineNo < 0 || lineNo >= lines.size()) return body;
    const QString line = lines[lineNo];
    const auto m = taskRe().match(line);
    if (!m.hasMatch()) return body;
    const auto parsed = parseTaskLine(line, lineNo);
    if (!parsed) return body;

    static const QRegularExpression doneStrip(QStringLiteral(R"(\s*✅️?\s*\d{4}-\d{2}-\d{2})"));
    if (parsed->done) {
        QString rest = m.captured(4);
        rest.remove(doneStrip);
        while (!rest.isEmpty() && rest.back() == u' ') rest.chop(1);
        lines[lineNo] = m.captured(1) + QStringLiteral(" ") + m.captured(3) + rest;
        return lines.join(u'\n');
    }

    // Checking an open task.
    if (!parsed->due && !parsed->repeat) {
        lines[lineNo] = m.captured(1) + QStringLiteral("x") + m.captured(3) + m.captured(4);
        return lines.join(u'\n');
    }

    QString rest = m.captured(4);
    rest.remove(doneStrip);
    while (!rest.isEmpty() && rest.back() == u' ') rest.chop(1);
    const QString doneLine = m.captured(1) + QStringLiteral("x") + m.captured(3) + rest + QStringLiteral(" ✅ ") + today.toString(Qt::ISODate);
    lines[lineNo] = doneLine;

    if (parsed->repeat && parsed->due) {
        const QDate nextDay = nextDate(parsed->due->date, *parsed->repeat);
        if (nextDay.isValid()) {
            const QString openBase = m.captured(1) + QStringLiteral(" ") + m.captured(3) + rest;
            const QString nextLine = setTaskDue(openBase, Due{nextDay, parsed->due->time});
            lines.insert(lineNo, nextLine);
        }
    }
    return lines.join(u'\n');
}

std::optional<Due> parseWhen(const QString &input, const QDateTime &now) {
    const QString raw = input.trimmed();
    if (raw.isEmpty()) return std::nullopt;

    // 1a. Chinese / mixed relative minutes or hours: 10分钟后, 半小时后, 1个半小时后, 过2小时
    {
        QString s = raw;
        const bool hasHalf = s.contains(u'半');
        QString probe = s;
        probe.remove(u'半');
        static const QRegularExpression relZh(
            QStringLiteral(R"(^(?:过)?(\d{1,4}|[零〇一二两三四五六七八九十]{1,3})?\s*(?:个)?\s*(分钟|分|小时|钟头|mins?|minutes?|hrs?|hours?|h)\s*(?:后|以后|之后|later)?$)"),
            QRegularExpression::CaseInsensitiveOption);
        if (const auto m = relZh.match(probe); m.hasMatch()) {
            double count = 0.0;
            if (!m.captured(1).isEmpty()) {
                const int n = parseNum(m.captured(1));
                if (n < 0) return std::nullopt;
                count = n;
            }
            if (hasHalf) count += 0.5;
            if (count > 0.0) {
                const QString u = m.captured(2).toLower();
                const bool isHour = u == QStringLiteral("小时") || u == QStringLiteral("钟头") || u.startsWith(u'h');
                const double mins = isHour ? count * 60.0 : count;
                if (mins > 0.0 && mins <= 10080.0) {
                    const qint64 targetMs = qint64(std::ceil((double(now.toMSecsSinceEpoch()) + mins * 60000.0) / 60000.0)) * 60000LL;
                    const QDateTime dt = QDateTime::fromMSecsSinceEpoch(targetMs);
                    return Due{dt.date(), QTime(dt.time().hour(), dt.time().minute())};
                }
            }
        }
    }

    // 1b. English relative minutes/hours: in 10 mins, in an hour, in half an hour
    {
        static const QRegularExpression relEn(
            QStringLiteral(R"(^in\s+(\d{1,4}|half\s+an?|an?)\s*(mins?|minutes?|hrs?|hours?|h)$)"),
            QRegularExpression::CaseInsensitiveOption);
        if (const auto m = relEn.match(raw); m.hasMatch()) {
            const QString amt = m.captured(1).toLower();
            double count = 0.0;
            if (amt.startsWith(QLatin1String("half"))) count = 0.5;
            else if (amt == QLatin1String("a") || amt == QLatin1String("an")) count = 1.0;
            else count = amt.toDouble();
            const QString u = m.captured(2).toLower();
            const bool isHour = u.startsWith(u'h');
            const double mins = isHour ? count * 60.0 : count;
            if (mins > 0.0 && mins <= 10080.0) {
                const qint64 targetMs = qint64(std::ceil((double(now.toMSecsSinceEpoch()) + mins * 60000.0) / 60000.0)) * 60000LL;
                const QDateTime dt = QDateTime::fromMSecsSinceEpoch(targetMs);
                return Due{dt.date(), QTime(dt.time().hour(), dt.time().minute())};
            }
        }
    }

    // 2. Time + Date extraction
    const TimeResult tr = takeTime(raw);
    const DateResult dr = takeDate(tr.rest, now.date());

    QString left = dr.rest;
    static const QRegularExpression filler(QStringLiteral(R"([\s,，的@在于到前]+)"));
    left.remove(filler);
    static const QRegularExpression prep(QStringLiteral(R"(^(?:at|on|by)$)"), QRegularExpression::CaseInsensitiveOption);
    left.remove(prep);
    if (!left.isEmpty()) return std::nullopt;

    if (dr.date.isValid()) return Due{dr.date, tr.time};
    if (tr.time.isValid()) {
        const QDate d = tr.time > now.time() ? now.date() : now.date().addDays(1);
        return Due{d, tr.time};
    }
    return std::nullopt;
}

QList<QuickDate> quickDates(const QDateTime &now, bool zh) {
    const QDate today = now.date();
    const QDate tomorrow = today.addDays(1);
    const int satDelta = (6 - today.dayOfWeek() + 7) % 7;
    const QDate weekend = today.addDays(satDelta == 0 ? 7 : satDelta);
    const QDate nextMon = today.addDays(8 - today.dayOfWeek());

    QList<QuickDate> out;
    out.append({zh ? QStringLiteral("今天") : QStringLiteral("Today"), Due{today, {}}});
    if (now.time().hour() < 20)
        out.append({zh ? QStringLiteral("今晚 20:00") : QStringLiteral("Tonight 20:00"), Due{today, QTime(20, 0)}});
    out.append({zh ? QStringLiteral("明天 09:00") : QStringLiteral("Tomorrow 09:00"), Due{tomorrow, QTime(9, 0)}});
    out.append({zh ? QStringLiteral("明天") : QStringLiteral("Tomorrow"), Due{tomorrow, {}}});
    out.append({zh ? QStringLiteral("周末") : QStringLiteral("This weekend"), Due{weekend, {}}});
    out.append({zh ? QStringLiteral("下周一") : QStringLiteral("Next Monday"), Due{nextMon, {}}});
    return out;
}

QString describeDue(const Due &due, const QDate &today, bool zh) {
    if (!due.valid()) return {};
    const qint64 days = today.daysTo(due.date);
    QString base;
    if (days == 0) base = zh ? QStringLiteral("今天") : QStringLiteral("Today");
    else if (days == 1) base = zh ? QStringLiteral("明天") : QStringLiteral("Tomorrow");
    else if (days == -1) base = zh ? QStringLiteral("昨天") : QStringLiteral("Yesterday");
    else if (zh) {
        base = (due.date.year() != today.year() ? QStringLiteral("%1年").arg(due.date.year()) : QString()) +
               QStringLiteral("%1月%2日").arg(due.date.month()).arg(due.date.day());
    } else {
        base = enMonthShort(due.date.month()) + u' ' + QString::number(due.date.day()) +
               (due.date.year() != today.year() ? QStringLiteral(", %1").arg(due.date.year()) : QString());
    }
    if (!due.allDay()) base += u' ' + due.timeString();
    return base;
}

QString describeDay(const QDate &date, const QDate &today, bool zh) {
    if (!date.isValid()) return {};
    const qint64 days = today.daysTo(date);
    const bool sameYear = date.year() == today.year();
    if (zh) {
        QString prefix;
        if (days == 0) prefix = QStringLiteral("今天 · ");
        else if (days == 1) prefix = QStringLiteral("明天 · ");
        return prefix + (sameYear ? QString() : QStringLiteral("%1年").arg(date.year())) +
               QStringLiteral("%1月%2日 %3").arg(date.month()).arg(date.day()).arg(zhWeekdayName(date.dayOfWeek()));
    }
    QString prefix;
    if (days == 0) prefix = QStringLiteral("Today · ");
    else if (days == 1) prefix = QStringLiteral("Tomorrow · ");
    return prefix + enWeekdayShort(date.dayOfWeek()) + QStringLiteral(", ") + enMonthShort(date.month()) + u' ' +
           QString::number(date.day()) + (sameYear ? QString() : QStringLiteral(", %1").arg(date.year()));
}

QString describeMonth(int year, int month, const QDate &today, bool zh) {
    if (year == today.year() && month == today.month())
        return zh ? QStringLiteral("本月稍后") : QStringLiteral("Later this month");
    if (zh) {
        return year == today.year() ? QStringLiteral("%1月").arg(month) : QStringLiteral("%1年%2月").arg(year).arg(month);
    }
    return year == today.year() ? enMonthFull(month) : QStringLiteral("%1 %2").arg(enMonthFull(month)).arg(year);
}

} // namespace tasks

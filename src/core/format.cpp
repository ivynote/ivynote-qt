#include "core/format.h"

#include "core/i18n.h"

#include <QDateTime>
#include <QLocale>
#include <cmath>

namespace fmt {

namespace {

QString hm(const QDateTime &d) { return d.toString(QStringLiteral("HH:mm")); }

QString monthDay(const QDateTime &d, bool withYear) {
    if (i18n::isZh())
        return (withYear ? QString::number(d.date().year()) + QStringLiteral("年") : QString()) +
               QStringLiteral("%1月%2日").arg(d.date().month()).arg(d.date().day());
    const QLocale en(QLocale::English);
    return en.toString(d.date(), withYear ? QStringLiteral("MMM d, yyyy") : QStringLiteral("MMM d"));
}

} // namespace

QString shortTime(qint64 ms, qint64 now) {
    if (!ms) return {};
    const QDateTime d = QDateTime::fromMSecsSinceEpoch(ms);
    const QDateTime n = QDateTime::fromMSecsSinceEpoch(now);
    if (d.date() == n.date()) return hm(d);
    if (d.date() == n.date().addDays(-1)) return i18n::t("昨天");
    if (now - ms < 6 * 86400000LL && ms < now) {
        static const char *zh[] = {"周一", "周二", "周三", "周四", "周五", "周六", "周日"};
        static const char *en[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
        const int i = d.date().dayOfWeek() - 1;
        return i18n::isZh() ? QString::fromUtf8(zh[i]) : QString::fromLatin1(en[i]);
    }
    return monthDay(d, d.date().year() != n.date().year());
}

QString fullTime(qint64 ms, qint64 now) {
    if (!ms) return {};
    const QDateTime d = QDateTime::fromMSecsSinceEpoch(ms);
    const bool sameYear = d.date().year() == QDateTime::fromMSecsSinceEpoch(now).date().year();
    return monthDay(d, !sameYear) + u' ' + hm(d);
}

QString ago(qint64 ms, qint64 now) {
    if (!ms) return i18n::t("从未");
    const qint64 s = std::max<qint64>(0, (now - ms + 500) / 1000);
    if (s < 45) return i18n::t("刚刚");
    const qint64 m = (s + 30) / 60;
    if (m < 60) return i18n::isZh() ? QStringLiteral("%1 分钟前").arg(m) : QStringLiteral("%1 min ago").arg(m);
    const qint64 h = (m + 30) / 60;
    if (h < 24) return i18n::isZh() ? QStringLiteral("%1 小时前").arg(h) : QStringLiteral("%1 h ago").arg(h);
    return shortTime(ms, now);
}

QString bytes(qint64 n) {
    if (n <= 0) return QStringLiteral("0 B");
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int i = std::min(4, int(std::floor(std::log(double(n)) / std::log(1024.0))));
    const double v = n / std::pow(1024.0, i);
    return (v >= 100 || i == 0 ? QString::number(qRound(v)) : QString::number(v, 'f', 1)) + u' ' + QLatin1String(units[i]);
}

QString number(qint64 n) {
    return QLocale(QLocale::English).toString(n);
}

} // namespace fmt

#include "core/hlc.h"

#include <QDateTime>

Hlc::Hlc(QString node, std::function<qint64()> now)
    : m_node(std::move(node)), m_now(now ? std::move(now) : [] { return QDateTime::currentMSecsSinceEpoch(); }) {}

QString Hlc::tick() {
    const qint64 ms = m_now();
    if (ms > m_wall) {
        m_wall = ms;
        m_counter = 0;
    } else {
        m_counter++;
    }
    return format(m_wall, m_counter, m_node);
}

void Hlc::observe(const QString &ts) {
    qint64 wall;
    int counter;
    if (!parse(ts, &wall, &counter)) return;
    if (wall > m_wall || (wall == m_wall && counter > m_counter)) {
        m_wall = wall;
        m_counter = counter;
    }
}

QString Hlc::format(qint64 wall, int counter, const QString &node) {
    return QStringLiteral("%1-%2-%3")
        .arg(wall, 13, 10, QLatin1Char('0'))
        .arg(counter & 0xffff, 4, 16, QLatin1Char('0'))
        .arg(node);
}

bool Hlc::parse(const QString &ts, qint64 *wall, int *counter, QString *node) {
    // The node may itself contain '-', so split only twice.
    if (ts.size() < 20 || ts.at(13) != u'-' || ts.at(18) != u'-') return false;
    bool ok1 = false, ok2 = false;
    const qint64 w = ts.left(13).toLongLong(&ok1, 10);
    const int c = ts.mid(14, 4).toInt(&ok2, 16);
    if (!ok1 || !ok2) return false;
    *wall = w;
    *counter = c;
    if (node) *node = ts.mid(19);
    return true;
}

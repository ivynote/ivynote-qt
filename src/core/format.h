// Date, size and number display, following the web client.
#pragma once

#include <QString>

class QDateTime;

namespace fmt {

QString shortTime(qint64 ms, qint64 now);  // 16:20 / 昨天 / 周三 / 10月29日 / 2025年3月1日
QString fullTime(qint64 ms, qint64 now);   // 10月31日 16:20
QString ago(qint64 ms, qint64 now);        // 刚刚 / 2 分钟前
QString bytes(qint64 n);
QString number(qint64 n);                  // 1,024

// Expands YYYY, YY, MM, DD, HH, mm, ss, ddd in a single pass (matches web/Tauri).
QString formatNoteTitle(const QString &tpl, const QDateTime &now, bool zh);

} // namespace fmt

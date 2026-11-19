// Slash command / insert menu catalog and fuzzy/pinyin matching, aligned with web/Tauri.
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace insertitems {

struct Item {
    QString id;
    QString section;
    QString zh;
    QString en;
    QString descZh;
    QString descEn;
    QString icon; // short glyph badge shown in menu
    QString hint; // markdown syntax hint on the right
    QStringList keys;
};

QString sectionLabel(const QString &section, bool zh);
const QList<Item> &all();
QList<Item> match(const QString &query);

} // namespace insertitems

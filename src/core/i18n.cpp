#include "core/i18n.h"

#include "core/i18n_en.h"

#include <QHash>
#include <QLocale>

namespace i18n {

namespace {

Lang g_lang = Lang::Zh;

const QHash<QString, QString> &english() {
    static const QHash<QString, QString> table = [] {
        QHash<QString, QString> t;
        for (const auto &p : kEnglish) t.insert(QString::fromUtf8(p.zh), QString::fromUtf8(p.en));
        return t;
    }();
    return table;
}

} // namespace

void setLang(Lang lang) { g_lang = lang; }
Lang lang() { return g_lang; }
bool isZh() { return g_lang == Lang::Zh; }

Lang systemLang() {
    // Chinese first: only an explicitly non-Chinese system gets English.
    const QLocale l = QLocale::system();
    return l.language() == QLocale::Chinese || l.language() == QLocale::C ? Lang::Zh : Lang::En;
}

QString t(const char *zh) { return t(QString::fromUtf8(zh)); }

QString t(const QString &zh) {
    if (g_lang == Lang::Zh) return zh;
    const auto it = english().constFind(zh);
    return it == english().constEnd() ? zh : *it;
}

} // namespace i18n

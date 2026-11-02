// Interface text. Chinese is the source language and the default; English
// comes from a table keyed by the Chinese text.
#pragma once

#include <QString>

namespace i18n {

enum class Lang { Zh, En };

void setLang(Lang lang);
Lang lang();
bool isZh();
Lang systemLang();

QString t(const char *zh);
QString t(const QString &zh);

} // namespace i18n

// Shorthand for interface strings.
inline QString T(const char *zh) { return i18n::t(zh); }
inline QString T(const QString &zh) { return i18n::t(zh); }

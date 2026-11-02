#include "core/ids.h"

#include <sodium.h>

namespace ids {

QString newId(int length) {
    static const bool ready = sodium_init() >= 0;
    Q_UNUSED(ready);
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    QString out;
    out.reserve(length);
    for (int i = 0; i < length; ++i) out += QLatin1Char(alphabet[randombytes_uniform(36)]);
    return out;
}

bool valid(const QString &id) {
    if (id.size() < 8 || id.size() > 64) return false;
    for (QChar c : id) {
        const ushort u = c.unicode();
        const bool ok = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '_' || u == '-';
        if (!ok) return false;
    }
    return true;
}

} // namespace ids

#pragma once

#include <QString>

namespace ids {

// 16 random [a-z0-9] characters from a cryptographically secure source.
QString newId(int length = 16);
bool valid(const QString &id); // ^[A-Za-z0-9_-]{8,64}$

} // namespace ids

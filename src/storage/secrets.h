// Login tokens and vault keys live in the system's secure storage: Secret
// Service on Linux, Credential Manager on Windows, Keychain on macOS.
//
// When no backend is available (a bare window manager without a secret
// service, say) values fall back to a file only this user can read, and
// insecure() reports it so the settings can say so.
#pragma once

#include <QByteArray>
#include <QString>

namespace secrets {

bool get(const QString &key, QByteArray *out); // false when missing or unavailable
bool set(const QString &key, const QByteArray &value);
void remove(const QString &key);
bool insecure();

// Tests and portable setups keep everything in the fallback file.
void setFallbackOnly(bool on);

} // namespace secrets

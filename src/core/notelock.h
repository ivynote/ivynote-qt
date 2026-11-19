// Password-protected notes (guide 8.4), byte-compatible with the web client's
// packages/core/src/crypto/noteLock.ts.
//
// A protected note's body is one ```ivy-encrypted fence holding a readable
// notice, a few header fields and the ciphertext. The plaintext only exists
// in the memory of the client that unlocked it; the server, local database,
// search and history see the fence. Key = scrypt(NFC(password), salt),
// cipher = XChaCha20-Poly1305, and the header is the associated data.
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <functional>

namespace notelock {

struct Kdf {
    quint32 N = 32768;
    quint32 r = 8;
    quint32 p = 1;
    bool operator==(const Kdf &o) const { return N == o.N && r == o.r && p == o.p; }
};

struct Envelope {
    Kdf kdf;
    QString salt; // base64, exactly as written (it is part of the AAD)
    QByteArray nonce;
    QString hint;
    QStringList refs;
    QByteArray data;
};

// A derived key, bound to the salt and parameters it was made with.
struct Key {
    QByteArray bytes;
    QString salt;
    Kdf kdf;
    QString hint;
    bool valid() const { return bytes.size() == 32; }
};

enum class Status { Ok, WrongPassword, Broken };

inline constexpr int kMinPassword = 4;
inline constexpr qint64 kKeyIdleMs = 10 * 60 * 1000;
inline const char *kLang = "ivy-encrypted";

// Whether a note body is a protected note. Cheap: a prefix test first.
bool isEncrypted(const QString &body);
bool parseEnvelope(const QString &body, Envelope *out);
// The password hint of a protected body ("" when none or not protected).
QString passwordHint(const QString &body);
// Attachment ids a body embeds, for the `refs` header.
QStringList resourceRefs(const QString &plain);

// Derives the key for a password and salt. Takes a moment on purpose
// (~0.1 s); run it off the UI thread. Invalid when the parameters are out of
// bounds or sodium fails.
Key deriveKey(const QString &password, const QString &salt, const Kdf &kdf, const QString &hint = {});
// A key for a new password: fresh salt.
Key newKey(const QString &password, const QString &hint = {});

// Encrypts Markdown: a fresh nonce every time. Cheap, so an editor can seal
// on every save. Empty string when the key is invalid.
QString seal(const QString &plain, const Key &key);
// Same with a fixed nonce, for tests only.
QString sealWithNonce(const QString &plain, const Key &key, const QByteArray &nonce);
Status open(const QString &body, const Key &key, QString *plain);
// Unlocks a body with a password: the plaintext and the key to seal edits.
Status unlock(const QString &body, const QString &password, QString *plain, Key *key);

// --- Keys of notes unlocked in this session, in memory only. Keyed by salt:
// a note and its conflict copies share a password. ---
void setClock(std::function<qint64()> now); // tests; null restores the real clock
void remember(const Key &key);
bool rememberedKey(const QString &body, Key *key); // false when locked or idle too long
bool openRemembered(const QString &body, QString *plain, Key *key);
void touch(const Key &key);
// Whether this key is still remembered and not idle too long. Unlike
// rememberedKey() it does not count as use, so a timer can poll it.
bool hasLiveKey(const Key &key);
// Forgets the key of one note (lock now).
void forget(const QString &body);
void forget(const Key &key);
void forgetAll();

} // namespace notelock

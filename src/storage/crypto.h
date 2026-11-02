// At-rest encryption of a vault on this device.
//
// The server already encrypts everything it stores and the API speaks
// plaintext (guide 8.1), so this never touches what goes over the wire. It
// only protects the local copy: every database record and every cached or
// queued file is sealed with XChaCha20-Poly1305 under a 256-bit vault key
// kept in the system keychain. Associated data binds each ciphertext to its
// place (table and id, or file name and chunk index), so records cannot be
// swapped or files truncated without detection.
#pragma once

#include <QByteArray>
#include <QString>

class VaultCipher {
public:
    VaultCipher() = default; // passthrough: an unencrypted vault
    explicit VaultCipher(const QByteArray &key);
    ~VaultCipher();
    VaultCipher(const VaultCipher &) = default;
    VaultCipher &operator=(const VaultCipher &) = default;

    bool active() const { return !m_key.isEmpty(); }

    QByteArray seal(const QByteArray &plain, const QByteArray &ad) const;
    bool open(const QByteArray &sealed, const QByteArray &ad, QByteArray *out) const;

    // Whole files, sealed in 1 MiB chunks; written atomically.
    bool writeFile(const QString &path, const QByteArray &data, const QString &name) const;
    bool readFile(const QString &path, const QString &name, QByteArray *out) const;

    static QByteArray generateKey();
    static QString encodeRecoveryKey(const QByteArray &key); // XXXX-XXXX-… base32
    static QByteArray decodeRecoveryKey(const QString &text); // empty on error

private:
    QByteArray m_key;
};

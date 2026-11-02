#include "storage/crypto.h"

#include <QFile>
#include <QSaveFile>
#include <QtEndian>
#include <cstring>
#include <sodium.h>

namespace {

constexpr int kNonce = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
constexpr int kTag = crypto_aead_xchacha20poly1305_ietf_ABYTES;
constexpr qsizetype kChunk = 1 << 20;
const QByteArray kMagic("IVF1");

bool ensureSodium() {
    static const bool ok = sodium_init() >= 0;
    return ok;
}

QByteArray chunkAd(const QString &name, quint32 index, bool final) {
    return QStringLiteral("ivy:file:v1|%1|%2|%3").arg(name).arg(index).arg(final ? 1 : 0).toUtf8();
}

const char kB32[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

} // namespace

VaultCipher::VaultCipher(const QByteArray &key) : m_key(key) {
    ensureSodium();
    if (m_key.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES) m_key.clear();
}

VaultCipher::~VaultCipher() {
    if (!m_key.isEmpty() && m_key.isDetached()) sodium_memzero(m_key.data(), size_t(m_key.size()));
}

QByteArray VaultCipher::seal(const QByteArray &plain, const QByteArray &ad) const {
    if (!active()) return plain;
    QByteArray out(kNonce + plain.size() + kTag, Qt::Uninitialized);
    auto *nonce = reinterpret_cast<unsigned char *>(out.data());
    randombytes_buf(nonce, kNonce);
    unsigned long long len = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(
        nonce + kNonce, &len, reinterpret_cast<const unsigned char *>(plain.constData()), size_t(plain.size()),
        reinterpret_cast<const unsigned char *>(ad.constData()), size_t(ad.size()), nullptr, nonce,
        reinterpret_cast<const unsigned char *>(m_key.constData()));
    out.resize(kNonce + qsizetype(len));
    return out;
}

bool VaultCipher::open(const QByteArray &sealed, const QByteArray &ad, QByteArray *out) const {
    if (!active()) {
        *out = sealed;
        return true;
    }
    if (sealed.size() < kNonce + kTag) return false;
    QByteArray plain(sealed.size() - kNonce - kTag, Qt::Uninitialized);
    unsigned long long len = 0;
    const auto *nonce = reinterpret_cast<const unsigned char *>(sealed.constData());
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            reinterpret_cast<unsigned char *>(plain.data()), &len, nullptr, nonce + kNonce,
            size_t(sealed.size() - kNonce), reinterpret_cast<const unsigned char *>(ad.constData()),
            size_t(ad.size()), nonce, reinterpret_cast<const unsigned char *>(m_key.constData())) != 0)
        return false;
    plain.resize(qsizetype(len));
    *out = plain;
    return true;
}

bool VaultCipher::writeFile(const QString &path, const QByteArray &data, const QString &name) const {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    if (!active()) {
        f.write(data);
        return f.commit();
    }
    f.write(kMagic);
    quint32 index = 0;
    qsizetype pos = 0;
    do {
        const qsizetype n = std::min(kChunk, data.size() - pos);
        const bool final = pos + n >= data.size();
        const QByteArray sealed = seal(data.mid(pos, n), chunkAd(name, index, final));
        char len[4];
        qToBigEndian<quint32>(quint32(sealed.size()), len);
        f.write(len, 4);
        f.write(sealed);
        pos += n;
        index++;
    } while (pos < data.size());
    return f.commit();
}

bool VaultCipher::readFile(const QString &path, const QString &name, QByteArray *out) const {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray raw = f.readAll();
    if (!active()) {
        *out = raw;
        return true;
    }
    if (!raw.startsWith(kMagic)) return false;
    QByteArray plain;
    qsizetype pos = kMagic.size();
    quint32 index = 0;
    while (pos + 4 <= raw.size()) {
        const quint32 len = qFromBigEndian<quint32>(raw.constData() + pos);
        pos += 4;
        if (pos + qsizetype(len) > raw.size()) return false;
        const bool final = pos + qsizetype(len) == raw.size();
        QByteArray chunk;
        if (!open(raw.mid(pos, len), chunkAd(name, index, final), &chunk)) return false;
        plain += chunk;
        pos += len;
        index++;
        if (final) {
            *out = plain;
            return true;
        }
    }
    return false; // truncated: the final chunk never came
}

QByteArray VaultCipher::generateKey() {
    ensureSodium();
    QByteArray key(crypto_aead_xchacha20poly1305_ietf_KEYBYTES, Qt::Uninitialized);
    crypto_aead_xchacha20poly1305_ietf_keygen(reinterpret_cast<unsigned char *>(key.data()));
    return key;
}

QString VaultCipher::encodeRecoveryKey(const QByteArray &key) {
    QString out;
    quint32 buffer = 0;
    int bits = 0;
    for (char c : key) {
        buffer = (buffer << 8) | quint8(c);
        bits += 8;
        while (bits >= 5) {
            out += QLatin1Char(kB32[(buffer >> (bits - 5)) & 31]);
            bits -= 5;
        }
    }
    if (bits > 0) out += QLatin1Char(kB32[(buffer << (5 - bits)) & 31]);
    QString grouped;
    for (int i = 0; i < out.size(); i += 4) {
        if (i) grouped += u'-';
        grouped += out.mid(i, 4);
    }
    return grouped;
}

QByteArray VaultCipher::decodeRecoveryKey(const QString &text) {
    QByteArray out;
    quint32 buffer = 0;
    int bits = 0;
    for (QChar ch : text.toUpper()) {
        if (ch == u'-' || ch.isSpace()) continue;
        const char *p = std::strchr(kB32, ch.toLatin1());
        if (!p || !ch.toLatin1()) return {};
        buffer = (buffer << 5) | quint32(p - kB32);
        bits += 5;
        if (bits >= 8) {
            out += char((buffer >> (bits - 8)) & 0xff);
            bits -= 8;
        }
    }
    return out.size() == crypto_aead_xchacha20poly1305_ietf_KEYBYTES ? out : QByteArray();
}

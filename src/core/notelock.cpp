#include "core/notelock.h"

#include <QDateTime>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QStringEncoder>
#include <sodium.h>

namespace notelock {

namespace {

constexpr int kNonce = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES; // 24
constexpr int kTag = crypto_aead_xchacha20poly1305_ietf_ABYTES;      // 16
const QString kFence = QStringLiteral("```");
const QString kNotice = QStringLiteral("This note is encrypted. Open it in ivyNote and enter its password.");

bool ensureSodium() {
    static const bool ok = sodium_init() >= 0;
    return ok;
}

const QRegularExpression &envelopeRe() {
    // The whole body is one fence, nothing before or after but blank space.
    static const QRegularExpression re(QStringLiteral(R"(^\s*```ivy-encrypted[ \t]*\r?\n([\s\S]*?)\r?\n```[ \t]*\s*\z)"));
    return re;
}

bool decodeB64(const QString &s, QByteArray *out) {
    const auto r = QByteArray::fromBase64Encoding(s.toLatin1(), QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!r) return false;
    *out = r.decoded;
    return true;
}

QString b64(const QByteArray &bytes) { return QString::fromLatin1(bytes.toBase64()); }

bool validKdf(const Kdf &k) {
    // Bounds keep a crafted note from asking for gigabytes of memory.
    return k.N >= 1024 && k.N <= (1u << 20) && (k.N & (k.N - 1)) == 0 && k.r >= 1 && k.r <= 16 && k.p >= 1 && k.p <= 4 &&
           quint64(k.N) * k.r * 128 <= quint64(256) * 1024 * 1024;
}

QString kdfText(const Kdf &k) { return QStringLiteral("scrypt N=%1 r=%2 p=%3").arg(k.N).arg(k.r).arg(k.p); }

QByteArray aad(const Kdf &kdf, const QString &salt, const QString &hint, const QStringList &refs) {
    return QStringList{QStringLiteral("ivy:note:v1"), kdfText(kdf), salt, hint, refs.join(u' ')}.join(u'|').toUtf8();
}

bool fits(const Envelope &env, const Key &key) { return env.salt == key.salt && env.kdf == key.kdf; }

QString headerLines(const Kdf &kdf, const QString &salt, const QString &hint, const QStringList &refs) {
    QStringList lines{kNotice, QStringLiteral("v: 1"), QStringLiteral("kdf: ") + kdfText(kdf), QStringLiteral("salt: ") + salt};
    if (!hint.isEmpty()) lines << QStringLiteral("hint: ") + b64(hint.toUtf8());
    if (!refs.isEmpty()) lines << QStringLiteral("refs: ") + refs.join(u' ');
    return lines.join(u'\n');
}

// --- remembered keys ---
struct Remembered {
    Key key;
    qint64 used = 0;
};
QMutex g_mutex;
QHash<QString, Remembered> g_keys;
std::function<qint64()> g_clock;

qint64 nowMs() { return g_clock ? g_clock() : QDateTime::currentMSecsSinceEpoch(); }

} // namespace

bool isEncrypted(const QString &body) {
    if (body.isEmpty()) return false;
    qsizetype i = 0;
    while (i < body.size() && body.at(i).isSpace()) ++i;
    if (!QStringView(body).mid(i).startsWith(kFence + QLatin1String(kLang))) return false;
    return envelopeRe().match(body).hasMatch();
}

bool parseEnvelope(const QString &body, Envelope *out) {
    const auto m = envelopeRe().match(body);
    if (!m.hasMatch()) return false;
    QHash<QString, QString> fields;
    static const QRegularExpression keyRe(QStringLiteral("^[a-z]+$"));
    for (const QString &line : m.captured(1).split(u'\n')) {
        const qsizetype i = line.indexOf(u':');
        if (i <= 0) continue;
        const QString k = line.left(i).trimmed();
        if (keyRe.match(k).hasMatch() && !fields.contains(k)) fields.insert(k, line.mid(i + 1).trimmed());
    }
    if (fields.value(QStringLiteral("v")) != QLatin1String("1")) return false;
    static const QRegularExpression kdfRe(QStringLiteral(R"(^scrypt N=(\d+) r=(\d+) p=(\d+)$)"));
    const auto km = kdfRe.match(fields.value(QStringLiteral("kdf")));
    if (!km.hasMatch()) return false;
    Envelope env;
    bool a = false, b = false, c = false;
    env.kdf.N = km.captured(1).toUInt(&a);
    env.kdf.r = km.captured(2).toUInt(&b);
    env.kdf.p = km.captured(3).toUInt(&c);
    if (!a || !b || !c) return false;
    env.salt = fields.value(QStringLiteral("salt"));
    if (env.salt.isEmpty() || fields.value(QStringLiteral("nonce")).isEmpty() || fields.value(QStringLiteral("data")).isEmpty()) return false;
    if (!decodeB64(fields.value(QStringLiteral("nonce")), &env.nonce)) return false;
    if (!decodeB64(fields.value(QStringLiteral("data")), &env.data)) return false;
    const QString hintText = fields.value(QStringLiteral("hint"));
    if (!hintText.isEmpty()) {
        QByteArray raw;
        if (decodeB64(hintText, &raw)) {
            QStringDecoder dec(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
            const QString text = dec(raw);
            if (!dec.hasError()) env.hint = text;
        }
    }
    static const QRegularExpression refRe(QStringLiteral("^[A-Za-z0-9_-]{8,64}$"));
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    for (const QString &r : fields.value(QStringLiteral("refs")).split(ws, Qt::SkipEmptyParts))
        if (refRe.match(r).hasMatch()) env.refs << r;
    *out = env;
    return true;
}

QString passwordHint(const QString &body) {
    Envelope env;
    return parseEnvelope(body, &env) ? env.hint : QString();
}

QStringList resourceRefs(const QString &plain) {
    static const QRegularExpression re(QStringLiteral(R"(ivy://res/([A-Za-z0-9_-]{8,64}))"));
    QStringList out;
    for (auto it = re.globalMatch(plain); it.hasNext();) {
        const QString id = it.next().captured(1);
        if (!out.contains(id)) out << id;
    }
    return out;
}

Key deriveKey(const QString &password, const QString &salt, const Kdf &kdf, const QString &hint) {
    Key key;
    QByteArray saltBytes;
    if (!ensureSodium() || !validKdf(kdf) || !decodeB64(salt, &saltBytes) || saltBytes.isEmpty()) return key;
    const QByteArray pw = password.normalized(QString::NormalizationForm_C).toUtf8();
    QByteArray out(32, Qt::Uninitialized);
    const int rc = crypto_pwhash_scryptsalsa208sha256_ll(reinterpret_cast<const uint8_t *>(pw.constData()), size_t(pw.size()),
                                                         reinterpret_cast<const uint8_t *>(saltBytes.constData()), size_t(saltBytes.size()),
                                                         kdf.N, kdf.r, kdf.p, reinterpret_cast<uint8_t *>(out.data()), size_t(out.size()));
    if (rc != 0) return key;
    key.bytes = out;
    key.salt = salt;
    key.kdf = kdf;
    key.hint = hint;
    return key;
}

Key newKey(const QString &password, const QString &hint) {
    if (!ensureSodium() || password.size() < kMinPassword) return {};
    QByteArray salt(16, Qt::Uninitialized);
    randombytes_buf(salt.data(), size_t(salt.size()));
    return deriveKey(password, b64(salt), Kdf{}, hint.trimmed().left(100));
}

QString sealWithNonce(const QString &plain, const Key &key, const QByteArray &nonce) {
    if (!key.valid() || nonce.size() != kNonce || !ensureSodium()) return {};
    const QStringList refs = resourceRefs(plain);
    const QByteArray ad = aad(key.kdf, key.salt, key.hint, refs);
    const QByteArray pt = plain.toUtf8();
    QByteArray ct(pt.size() + kTag, Qt::Uninitialized);
    unsigned long long clen = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(reinterpret_cast<uint8_t *>(ct.data()), &clen, reinterpret_cast<const uint8_t *>(pt.constData()),
                                               (unsigned long long)pt.size(), reinterpret_cast<const uint8_t *>(ad.constData()),
                                               (unsigned long long)ad.size(), nullptr, reinterpret_cast<const uint8_t *>(nonce.constData()),
                                               reinterpret_cast<const uint8_t *>(key.bytes.constData()));
    ct.resize(qsizetype(clen));
    return kFence + QLatin1String(kLang) + u'\n' + headerLines(key.kdf, key.salt, key.hint, refs) + u'\n' + QStringLiteral("nonce: ") + b64(nonce) +
           u'\n' + QStringLiteral("data: ") + b64(ct) + u'\n' + kFence + u'\n';
}

QString seal(const QString &plain, const Key &key) {
    if (!ensureSodium()) return {};
    QByteArray nonce(kNonce, Qt::Uninitialized);
    randombytes_buf(nonce.data(), size_t(nonce.size()));
    return sealWithNonce(plain, key, nonce);
}

Status open(const QString &body, const Key &key, QString *plain) {
    Envelope env;
    if (!ensureSodium() || !parseEnvelope(body, &env) || env.nonce.size() != kNonce || env.data.size() < kTag) return Status::Broken;
    if (!key.valid() || !fits(env, key)) return Status::WrongPassword;
    const QByteArray ad = aad(env.kdf, env.salt, env.hint, env.refs);
    QByteArray pt(env.data.size() - kTag, Qt::Uninitialized);
    unsigned long long plen = 0;
    const int rc = crypto_aead_xchacha20poly1305_ietf_decrypt(
        reinterpret_cast<uint8_t *>(pt.data()), &plen, nullptr, reinterpret_cast<const uint8_t *>(env.data.constData()),
        (unsigned long long)env.data.size(), reinterpret_cast<const uint8_t *>(ad.constData()), (unsigned long long)ad.size(),
        reinterpret_cast<const uint8_t *>(env.nonce.constData()), reinterpret_cast<const uint8_t *>(key.bytes.constData()));
    if (rc != 0) return Status::WrongPassword;
    pt.resize(qsizetype(plen));
    QStringDecoder dec(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    const QString text = dec(pt);
    sodium_memzero(pt.data(), size_t(pt.size()));
    if (dec.hasError()) return Status::Broken;
    *plain = text;
    return Status::Ok;
}

Status unlock(const QString &body, const QString &password, QString *plain, Key *key) {
    Envelope env;
    if (!parseEnvelope(body, &env)) return Status::Broken;
    const Key k = deriveKey(password, env.salt, env.kdf, env.hint);
    if (!k.valid()) return Status::Broken;
    const Status s = open(body, k, plain);
    if (s != Status::Ok) return s;
    remember(k);
    if (key) *key = k;
    return Status::Ok;
}

void setClock(std::function<qint64()> now) {
    QMutexLocker lock(&g_mutex);
    g_clock = std::move(now);
}

void remember(const Key &key) {
    QMutexLocker lock(&g_mutex);
    g_keys.insert(key.salt, {key, nowMs()});
}

bool rememberedKey(const QString &body, Key *key) {
    Envelope env;
    if (!parseEnvelope(body, &env)) return false;
    QMutexLocker lock(&g_mutex);
    auto it = g_keys.find(env.salt);
    if (it == g_keys.end()) return false;
    if (nowMs() - it->used > kKeyIdleMs) {
        g_keys.erase(it);
        return false;
    }
    if (!fits(env, it->key)) return false;
    it->used = nowMs();
    *key = it->key;
    return true;
}

bool openRemembered(const QString &body, QString *plain, Key *key) {
    Key k;
    if (!rememberedKey(body, &k)) return false;
    if (open(body, k, plain) != Status::Ok) return false;
    if (key) *key = k;
    return true;
}

void touch(const Key &key) {
    QMutexLocker lock(&g_mutex);
    auto it = g_keys.find(key.salt);
    if (it != g_keys.end()) it->used = nowMs();
}

bool hasLiveKey(const Key &key) {
    QMutexLocker lock(&g_mutex);
    auto it = g_keys.find(key.salt);
    if (it == g_keys.end()) return false;
    if (nowMs() - it->used > kKeyIdleMs) {
        g_keys.erase(it);
        return false;
    }
    return true;
}

void forget(const Key &key) {
    QMutexLocker lock(&g_mutex);
    g_keys.remove(key.salt);
}

void forget(const QString &body) {
    Envelope env;
    if (!parseEnvelope(body, &env)) return;
    QMutexLocker lock(&g_mutex);
    g_keys.remove(env.salt);
}

void forgetAll() {
    QMutexLocker lock(&g_mutex);
    g_keys.clear();
}

} // namespace notelock

#include "core/notelock.h"

#include <QtTest>

using namespace notelock;

namespace {

// Produced by the web client's noteLock.ts logic (@noble/ciphers, @noble/hashes)
// with salt 01..10 and nonce 100..123, password "pässwörd" (NFC).
const char kSmall[] =
    "```ivy-encrypted\nThis note is encrypted. Open it in ivyNote and enter its password.\nv: 1\nkdf: scrypt N=1024 r=8 p=1\n"
    "salt: AQIDBAUGBwgJCgsMDQ4PEA==\nhint: 5a6g54mp5ZCN\nrefs: abcdefgh12345678\nnonce: ZGVmZ2hpamtsbW5vcHFyc3R1dnd4eXp7\n"
    "data: ACIH48TWqMZQFIJMoS++DwxcldlQtIgtTGn0nUsksELCLbMSEaDgFQYyJ1ekWqVUnRPv9zUtsmHpGV/9lszc1aWUZq0V\n```\n";
const char kFull[] =
    "```ivy-encrypted\nThis note is encrypted. Open it in ivyNote and enter its password.\nv: 1\nkdf: scrypt N=32768 r=8 p=1\n"
    "salt: AQIDBAUGBwgJCgsMDQ4PEA==\nrefs: abcdefgh12345678\nnonce: ZGVmZ2hpamtsbW5vcHFyc3R1dnd4eXp7\n"
    "data: gcLNCfdauQiGKkoMuwFJskQzefLJWzf5qiKDt+twQCkrKBEGBIAzQr6PKPEMcNQ0nu5PzyLR8IWKklhlOaoDMpjvrw3q\n```\n";
const QString kPlain = QString::fromUtf8("# 秘密\n\n正文 ![](ivy://res/abcdefgh12345678) end\n");
// "pässwörd" with precomposed and decomposed characters must be the same password.
const QString kPassword = QString::fromUtf8("p\u00e4ssw\u00f6rd");
const QString kPasswordNfd = QString::fromUtf8("pa\u0308sswo\u0308rd");

} // namespace

class NoteLockTest : public QObject {
    Q_OBJECT
private slots:
    void cleanup() {
        forgetAll();
        setClock(nullptr);
    }

    void detectsEnvelope() {
        QVERIFY(isEncrypted(QString::fromLatin1(kSmall)));
        QVERIFY(isEncrypted(QStringLiteral("\n  \n") + QString::fromLatin1(kSmall) + QStringLiteral("\n\n")));
        QVERIFY(!isEncrypted(QString()));
        QVERIFY(!isEncrypted(QStringLiteral("# plain")));
        QVERIFY(!isEncrypted(QStringLiteral("text\n") + QString::fromLatin1(kSmall)));    // text before the fence
        QVERIFY(!isEncrypted(QString::fromLatin1(kSmall) + QStringLiteral("text after"))); // text after it
        QVERIFY(!isEncrypted(QStringLiteral("```ivy-encrypted\nno end")));
        QVERIFY(!isEncrypted(QStringLiteral("```text\nv: 1\n```")));
    }

    void parsesHeader() {
        Envelope env;
        QVERIFY(parseEnvelope(QString::fromLatin1(kSmall), &env));
        QCOMPARE(env.kdf.N, 1024u);
        QCOMPARE(env.kdf.r, 8u);
        QCOMPARE(env.kdf.p, 1u);
        QCOMPARE(env.hint, QString::fromUtf8("宠物名"));
        QCOMPARE(env.refs, QStringList{QStringLiteral("abcdefgh12345678")});
        QCOMPARE(env.nonce.size(), 24);
        QCOMPARE(passwordHint(QString::fromLatin1(kSmall)), QString::fromUtf8("宠物名"));
        QCOMPARE(passwordHint(QString::fromLatin1(kFull)), QString());
        QCOMPARE(passwordHint(QStringLiteral("plain")), QString());
        // Unknown versions and broken kdf lines are refused, not guessed.
        QString v2 = QString::fromLatin1(kSmall);
        v2.replace(QStringLiteral("v: 1"), QStringLiteral("v: 2"));
        QVERIFY(!parseEnvelope(v2, &env));
        QString bad = QString::fromLatin1(kSmall);
        bad.replace(QStringLiteral("kdf: scrypt N=1024 r=8 p=1"), QStringLiteral("kdf: pbkdf2"));
        QVERIFY(!parseEnvelope(bad, &env));
    }

    void opensWebClientNotes() {
        QString plain;
        Key key;
        QCOMPARE(unlock(QString::fromLatin1(kSmall), kPassword, &plain, &key), Status::Ok);
        QCOMPARE(plain, kPlain);
        QVERIFY(key.valid());
        QCOMPARE(key.hint, QString::fromUtf8("宠物名"));
        // Unicode normalization: the same password typed as NFD.
        QString again;
        QCOMPARE(unlock(QString::fromLatin1(kSmall), kPasswordNfd, &again, nullptr), Status::Ok);
        QCOMPARE(again, kPlain);
        // The default parameters (N=32768) too.
        QString full;
        QCOMPARE(unlock(QString::fromLatin1(kFull), kPassword, &full, nullptr), Status::Ok);
        QCOMPARE(full, kPlain);
    }

    void wrongPassword() {
        QString plain;
        QCOMPARE(unlock(QString::fromLatin1(kSmall), QStringLiteral("nope"), &plain, nullptr), Status::WrongPassword);
        QVERIFY(plain.isEmpty());
        QCOMPARE(unlock(QStringLiteral("plain"), QStringLiteral("x"), &plain, nullptr), Status::Broken);
    }

    void sealMatchesWebClientByteForByte() {
        Envelope env;
        QVERIFY(parseEnvelope(QString::fromLatin1(kSmall), &env));
        const Key key = deriveKey(kPassword, env.salt, env.kdf, env.hint);
        QVERIFY(key.valid());
        QCOMPARE(sealWithNonce(kPlain, key, env.nonce), QString::fromLatin1(kSmall));
    }

    void sealOpenRoundTrip() {
        const Key key = newKey(QStringLiteral("correct horse"), QString::fromUtf8("  提示  "));
        QVERIFY(key.valid());
        QCOMPARE(key.hint, QString::fromUtf8("提示")); // trimmed
        const QString body = seal(kPlain, key);
        QVERIFY(isEncrypted(body));
        QVERIFY(!body.contains(QStringLiteral("秘密")));
        QVERIFY(body.contains(QStringLiteral("refs: abcdefgh12345678")));
        QString out;
        QCOMPARE(open(body, key, &out), Status::Ok);
        QCOMPARE(out, kPlain);
        // A fresh nonce on every save, the salt stays.
        const QString second = seal(kPlain, key);
        QVERIFY(second != body);
        Envelope a, b;
        QVERIFY(parseEnvelope(body, &a) && parseEnvelope(second, &b));
        QCOMPARE(a.salt, b.salt);
        QVERIFY(a.nonce != b.nonce);
        // Empty and large notes.
        QCOMPARE(open(seal(QString(), key), key, &out), Status::Ok);
        QVERIFY(out.isEmpty());
        const QString big = QString(300000, QChar(u'汉'));
        QCOMPARE(open(seal(big, key), key, &out), Status::Ok);
        QCOMPARE(out, big);
    }

    void shortPasswordRefused() { QVERIFY(!newKey(QStringLiteral("abc")).valid()); }

    void tamperingIsDetected() {
        const Key key = newKey(QStringLiteral("secret pw"), QStringLiteral("hint"));
        const QString body = seal(kPlain, key);
        QString out;
        // Hint swapped for another one: authenticated header, so it fails.
        QString swapped = body;
        swapped.replace(QStringLiteral("hint: ") + QString::fromLatin1(QByteArray("hint").toBase64()), QStringLiteral("hint: ") + QString::fromLatin1(QByteArray("evil").toBase64()));
        QVERIFY(swapped != body);
        QCOMPARE(open(swapped, key, &out), Status::WrongPassword);
        // A refs list rewritten.
        QString refs = body;
        refs.replace(QStringLiteral("refs: abcdefgh12345678"), QStringLiteral("refs: zzzzzzzzzzzz"));
        QCOMPARE(open(refs, key, &out), Status::WrongPassword);
        // Flipped ciphertext byte.
        QString flipped = body;
        const qsizetype at = flipped.indexOf(QStringLiteral("data: ")) + 8;
        flipped[at] = flipped.at(at) == u'A' ? u'B' : u'A';
        QCOMPARE(open(flipped, key, &out), Status::WrongPassword);
        QVERIFY(out.isEmpty());
    }

    void invalidKdfParametersAreRefused() {
        // A crafted note must not make the client allocate gigabytes.
        QVERIFY(!deriveKey(QStringLiteral("x"), QStringLiteral("AQIDBAUGBwgJCgsMDQ4PEA=="), Kdf{1u << 20, 16, 4}).valid());
        QVERIFY(!deriveKey(QStringLiteral("x"), QStringLiteral("AQIDBAUGBwgJCgsMDQ4PEA=="), Kdf{1000, 8, 1}).valid()); // not a power of two
        QVERIFY(!deriveKey(QStringLiteral("x"), QStringLiteral("AQIDBAUGBwgJCgsMDQ4PEA=="), Kdf{16, 8, 1}).valid());   // too weak
        QVERIFY(!deriveKey(QStringLiteral("x"), QStringLiteral("***not base64***"), Kdf{1024, 8, 1}).valid());
    }

    void keyCacheExpiresAndForgets() {
        qint64 t = 1000;
        setClock([&] { return t; });
        const QString body = QString::fromLatin1(kSmall);
        QString plain;
        Key key;
        QVERIFY(!openRemembered(body, &plain, &key));
        QCOMPARE(unlock(body, kPassword, &plain, nullptr), Status::Ok);
        QVERIFY(openRemembered(body, &plain, &key));
        QCOMPARE(plain, kPlain);
        // Use keeps the key alive; ten idle minutes drop it.
        t += kKeyIdleMs - 1;
        QVERIFY(openRemembered(body, &plain, &key));
        t += kKeyIdleMs - 1;
        QVERIFY(openRemembered(body, &plain, &key));
        t += kKeyIdleMs + 1;
        QVERIFY(!openRemembered(body, &plain, &key));
        // Lock now.
        QCOMPARE(unlock(body, kPassword, &plain, nullptr), Status::Ok);
        forget(body);
        QVERIFY(!openRemembered(body, &plain, &key));
        // The same salt covers a re-sealed body (conflict copies share a password).
        QCOMPARE(unlock(body, kPassword, &plain, &key), Status::Ok);
        const QString resealed = seal(QStringLiteral("edited"), key);
        QVERIFY(openRemembered(resealed, &plain, &key));
        QCOMPARE(plain, QStringLiteral("edited"));
    }

    void resourceRefsAreUniqueAndBounded() {
        QCOMPARE(resourceRefs(QStringLiteral("![](ivy://res/aaaaaaaa) ![](ivy://res/aaaaaaaa) ![](ivy://res/bbbbbbbb1)")),
                 (QStringList{QStringLiteral("aaaaaaaa"), QStringLiteral("bbbbbbbb1")}));
        QVERIFY(resourceRefs(QStringLiteral("ivy://res/short")).isEmpty());
    }
};

QTEST_MAIN(NoteLockTest)
#include "test_notelock.moc"

#include "storage/secrets.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <qt6keychain/keychain.h>

namespace secrets {

namespace {

const QString kService = QStringLiteral("Ivynote");
bool g_fallbackOnly = false;
bool g_insecure = false;

QString fallbackPath() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/secrets.json");
}

QJsonObject readFallback() {
    QFile f(fallbackPath());
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

void writeFallback(const QJsonObject &o) {
    QSaveFile f(fallbackPath());
    if (!f.open(QIODevice::WriteOnly)) return;
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
    f.commit();
}

template <typename Job>
QKeychain::Error run(Job &job) {
    QEventLoop loop;
    job.setAutoDelete(false);
    QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job.start();
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    return job.error();
}

} // namespace

bool get(const QString &key, QByteArray *out) {
    if (!g_fallbackOnly) {
        QKeychain::ReadPasswordJob job(kService);
        job.setKey(key);
        const auto err = run(job);
        if (err == QKeychain::NoError) {
            *out = job.binaryData();
            return true;
        }
        // Missing or no backend: it may still be in the fallback file.
    }
    const QJsonObject o = readFallback();
    if (!o.contains(key)) return false;
    *out = QByteArray::fromBase64(o.value(key).toString().toLatin1());
    return true;
}

bool set(const QString &key, const QByteArray &value) {
    if (!g_fallbackOnly) {
        QKeychain::WritePasswordJob job(kService);
        job.setKey(key);
        job.setBinaryData(value);
        if (run(job) == QKeychain::NoError) {
            // Drop any fallback copy now that the keychain holds it.
            QJsonObject o = readFallback();
            if (o.contains(key)) {
                o.remove(key);
                writeFallback(o);
            }
            return true;
        }
    }
    g_insecure = !g_fallbackOnly;
    QJsonObject o = readFallback();
    o.insert(key, QString::fromLatin1(value.toBase64()));
    writeFallback(o);
    return true;
}

void remove(const QString &key) {
    if (!g_fallbackOnly) {
        QKeychain::DeletePasswordJob job(kService);
        job.setKey(key);
        run(job);
    }
    QJsonObject o = readFallback();
    if (o.contains(key)) {
        o.remove(key);
        writeFallback(o);
    }
}

bool insecure() { return g_insecure || (!g_fallbackOnly && !readFallback().isEmpty()); }

void setFallbackOnly(bool on) { g_fallbackOnly = on; }

} // namespace secrets

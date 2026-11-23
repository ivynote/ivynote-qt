#include "vault/session.h"

#include "storage/crypto.h"
#include "storage/localstore.h"
#include "storage/secrets.h"
#include "sync/api.h"
#include "sync/engine.h"
#include "sync/events.h"
#include "sync/transport.h"
#include "vault/files.h"

#include <QBuffer>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonObject>
#include <QMetaObject>
#include <QNetworkInformation>
#include <QPointer>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <algorithm>

Session::Session(const VaultInfo &info, QObject *parent) : QObject(parent), m_info(info) {
    m_images.setMaxCost(256 * 1024); // KiB of decoded pixels
    m_imagePool.setMaxThreadCount(std::clamp(QThread::idealThreadCount(), 2, 4));
}

Session *Session::open(const VaultInfo &info, const QByteArray &keyOverride, OpenError *err, QString *message, QObject *parent) {
    *err = OpenError::None;
    QByteArray key;
    if (info.encrypted) {
        if (!keyOverride.isEmpty()) key = keyOverride;
        else if (!secrets::get(info.vaultKeyKey(), &key) || key.isEmpty()) {
            *err = OpenError::MissingKey;
            return nullptr;
        }
    }
    const VaultCipher cipher(key);
    QDir().mkpath(info.dir());
    auto *s = new Session(info, parent);
    s->m_store = new LocalStore(info.dir() + QStringLiteral("/vault.db"), cipher);
    switch (s->m_store->open(message)) {
    case LocalStore::OpenResult::Ok: break;
    case LocalStore::OpenResult::WrongKey:
        *err = OpenError::WrongKey;
        delete s;
        return nullptr;
    case LocalStore::OpenResult::Failed:
        *err = OpenError::Failed;
        delete s;
        return nullptr;
    }
    if (!keyOverride.isEmpty()) secrets::set(info.vaultKeyKey(), keyOverride);
    if (!QFileInfo::exists(info.dir() + QStringLiteral("/vault.json"))) vaults::writeMetadata(info);
    s->m_files = new VaultFiles(info.dir(), cipher);
    if (info.isRemote()) {
        QSettings st;
        const bool localFirst = st.value(QStringLiteral("cache/attachmentLocalFirst"), true).toBool();
        const qint64 maxBytes = st.value(QStringLiteral("cache/attachmentMaxBytes"), VaultFiles::kDefaultMaxCacheBytes).toLongLong();
        s->m_files->setCachePolicy(true, localFirst, maxBytes);
    }
    const QString device = vaults::deviceId();
    if (info.isRemote()) {
        s->m_api = new ApiClient(QUrl(info.server), s);
        s->m_api->setDevice(device);
        QByteArray token;
        if (secrets::get(info.tokenKey(), &token)) s->m_api->setToken(QString::fromUtf8(token));
        s->m_transport = new ApiTransport(s->m_api);
        s->m_events = new EventStream(s->m_api, s);
        s->m_user = User::fromJson(info.user);
    }
    s->m_engine = new SyncEngine(s->m_store, s->m_files, s->m_transport, device, s);
    s->m_engine->init();

    connect(s->m_engine, &SyncEngine::resourceNormalized, s, [s](const QString &localId, const QString &remoteId) {
        // Transfer decoded in-memory images from `local-` to canonical `remoteId`
        // so the editor never flashes or re-decodes when links normalize.
        for (const QString &suffix : {QString(), QStringLiteral("#t")}) {
            if (const QImage *img = s->m_images.object(localId + suffix)) {
                const QImage copy = *img;
                s->m_images.insert(remoteId + suffix, new QImage(copy), std::max<qsizetype>(1, copy.sizeInBytes() / 1024));
            }
        }
        if (const QImage *full = s->m_images.object(localId); full && !s->m_images.contains(remoteId + QStringLiteral("#t"))) {
            const QImage copy = *full;
            s->m_images.insert(remoteId + QStringLiteral("#t"), new QImage(copy), std::max<qsizetype>(1, copy.sizeInBytes() / 1024));
        }
        emit s->attachmentReady(remoteId);
    });

    if (s->m_events) {
        connect(s->m_events, &EventStream::seq, s->m_engine, &SyncEngine::onServerSeq);
        connect(s->m_events, &EventStream::connectedChanged, s, &Session::liveChanged);
        connect(s->m_engine, &SyncEngine::unauthorized, s, [s] {
            s->m_events->stop();
            emit s->needsSignIn();
        });
    }
    return s;
}

Session::~Session() { close(); }

void Session::close() {
    if (m_closed) return;
    m_closed = true;
    m_cancelled->store(true);
    m_imagePool.clear();
    m_imagePool.waitForDone();
    if (m_events) m_events->stop();
    if (m_engine) {
        m_engine->stop();
        delete m_engine;
        m_engine = nullptr;
    }
    delete m_transport;
    m_transport = nullptr;
    if (m_files) m_files->flushIndex();
    delete m_files;
    m_files = nullptr;
    delete m_store;
    m_store = nullptr;
}

bool Session::signedIn() const { return !m_api || !m_api->token().isEmpty(); }
bool Session::liveConnected() const { return m_events && m_events->connected(); }

QByteArray Session::vaultKey() const {
    QByteArray key;
    if (m_info.encrypted) secrets::get(m_info.vaultKeyKey(), &key);
    return key;
}

int Session::unreadableRecords() const { return m_store ? m_store->unreadable() : 0; }

void Session::start() {
    if (!isRemote()) return;
    m_info.openedAt = QDateTime::currentMSecsSinceEpoch();
    vaults::save(m_info);
    if (!signedIn()) {
        m_engine->pause();
        QTimer::singleShot(0, this, &Session::needsSignIn);
        return;
    }
    m_engine->sync();
    m_events->start();
    refreshMe();
    if (QNetworkInformation::loadBackendByFeatures(QNetworkInformation::Feature::Reachability)) {
        connect(QNetworkInformation::instance(), &QNetworkInformation::reachabilityChanged, this,
                [this](QNetworkInformation::Reachability r) {
                    if (r == QNetworkInformation::Reachability::Online) wake();
                });
    }
}

void Session::wake() {
    if (!isRemote() || !signedIn() || m_closed || m_engine->paused()) return;
    m_failedImages.clear();
    m_engine->sync();
    m_events->reconnectNow();
    refreshMe();
}

void Session::refreshMe() {
    QPointer<Session> self(this);
    m_api->get(QStringLiteral("/me"), this, [self](const ApiReply &r) {
        if (!self) return;
        if (r.ok) {
            self->m_user = User::fromJson(r.json.toObject());
            self->m_info.user = self->m_user.toJson();
            vaults::save(self->m_info);
            emit self->userChanged();
        } else if (r.status == 401 || r.status == 403) {
            // Only these end the session; offline and 5xx keep the cached user.
            self->m_engine->pause();
            self->m_events->stop();
            emit self->needsSignIn();
        }
    });
}

void Session::rename(const QString &name) {
    m_info.name = name.trimmed();
    vaults::save(m_info);
}

bool Session::applySignIn(const QString &token, const User &user, QString *error) {
    if (!m_info.userId.isEmpty() && user.id != m_info.userId) {
        if (error) *error = QStringLiteral("other-account");
        return false;
    }
    secrets::set(m_info.tokenKey(), token.toUtf8());
    m_api->setToken(token);
    m_user = user;
    m_info.user = user.toJson();
    m_info.userId = user.id;
    m_info.username = user.username;
    vaults::save(m_info);
    emit userChanged();
    m_engine->resume();
    m_engine->sync();
    m_events->start();
    return true;
}

void Session::signOut(std::function<void()> done) {
    if (!isRemote()) {
        done();
        return;
    }
    QPointer<Session> self(this);
    auto finish = [self, done] {
        if (!self) return;
        if (self->signedIn()) {
            auto fired = std::make_shared<bool>(false);
            auto once = [fired, done] {
                if (*fired) return;
                *fired = true;
                done();
            };
            self->m_api->postJson(QStringLiteral("/auth/logout"), QJsonObject(), self, [once](const ApiReply &) { once(); }, 10000);
            QTimer::singleShot(11000, self, once);
        } else {
            done();
        }
    };
    if (!signedIn()) {
        finish();
        return;
    }
    // Sync first, but never wait forever on a server that does not answer.
    auto fired = std::make_shared<bool>(false);
    auto once = [fired, finish] {
        if (*fired) return;
        *fired = true;
        finish();
    };
    connect(m_engine, &SyncEngine::roundFinished, this, [once](bool) { once(); }, Qt::SingleShotConnection);
    QTimer::singleShot(15000, this, once);
    m_engine->sync();
}

// --- attachments ---

void Session::applyCachePolicy(bool localFirst, qint64 maxBytes) {
    if (!m_files || !isRemote()) return;
    m_files->setCachePolicy(true, localFirst, maxBytes);
}

QImage Session::decode(const QByteArray &data, int maxSide) {
    QBuffer buf;
    buf.setData(data);
    buf.open(QIODevice::ReadOnly);
    QImageReader reader(&buf);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (size.isValid() && (size.width() > maxSide || size.height() > maxSide))
        reader.setScaledSize(size.scaled(maxSide, maxSide, Qt::KeepAspectRatio));
    QImage img = reader.read();
    if (!img.isNull() && std::max(img.width(), img.height()) > maxSide)
        img = img.scaled(maxSide, maxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return img;
}

QImage Session::cachedImage(const QString &id, bool thumb) const {
    const QString cid = m_files ? m_files->canonicalId(id) : id;
    const QString key = cid + (thumb ? QStringLiteral("#t") : QString());
    if (const QImage *img = m_images.object(key)) return *img;
    if (thumb) {
        if (const QImage *full = m_images.object(cid)) return *full;
    }
    return {};
}

QImage Session::image(const QString &id, bool thumb) {
    const QString cid = m_files ? m_files->canonicalId(id) : id;
    const QString key = cid + (thumb ? QStringLiteral("#t") : QString());
    if (const QImage *img = m_images.object(key)) return *img;
    if (thumb) {
        if (const QImage *full = m_images.object(cid)) return *full;
    }
    if (m_loading.contains(key) || m_failedImages.contains(key) || m_closed || !m_files) return {};
    m_loading.insert(key);
    const int maxSide = thumb ? 1200 : 1600;
    QPointer<Session> self(this);
    const VaultFiles::DiskSource disk = m_files->diskSource();
    const auto cancelled = m_cancelled;

    auto finishImage = [self, key, id, cid](const QImage &img, bool fromCache, bool usedThumb) {
        if (!self || self->m_closed) return;
        self->m_loading.remove(key);
        if (fromCache && self->m_files) self->m_files->touchAccess(cid, usedThumb);
        if (img.isNull()) {
            self->m_failedImages.insert(key);
            return;
        }
        self->m_images.insert(key, new QImage(img), std::max<qsizetype>(1, img.sizeInBytes() / 1024));
        emit self->attachmentReady(id);
        if (cid != id) emit self->attachmentReady(cid);
    };

    auto decodeAsync = [self, key, maxSide, cancelled, finishImage](bool ok, QByteArray data) {
        if (!self || self->m_closed) return;
        if (!ok || data.isEmpty()) {
            self->m_loading.remove(key);
            return;
        }
        self->m_imagePool.start([self, data = std::move(data), maxSide, cancelled, finishImage] {
            if (cancelled->load()) return;
            const QImage img = Session::decode(data, maxSide);
            if (cancelled->load() || !self) return;
            QMetaObject::invokeMethod(self, [finishImage, img] { finishImage(img, false, false); }, Qt::QueuedConnection);
        });
    };

    m_imagePool.start([self, disk, id, cid, key, thumb, maxSide, cancelled, finishImage, decodeAsync] {
        if (cancelled->load()) return;
        QByteArray data;
        bool found = false;
        bool fromCache = false;
        bool usedThumb = thumb;
        if (id.startsWith(QLatin1String("local-")) && VaultFiles::readRaw(disk, QStringLiteral("uploads"), id, &data)) {
            found = true;
        } else if (disk.allowCacheRead) {
            if (VaultFiles::readRaw(disk, thumb ? QStringLiteral("thumbs") : QStringLiteral("files"), cid, &data)) {
                found = true;
                fromCache = true;
                usedThumb = thumb;
            } else if (thumb && VaultFiles::readRaw(disk, QStringLiteral("files"), cid, &data)) {
                found = true;
                fromCache = true;
                usedThumb = false;
            }
        }
        if (cancelled->load()) return;
        if (found) {
            const QImage img = Session::decode(data, maxSide);
            if (cancelled->load() || !self) return;
            QMetaObject::invokeMethod(self, [finishImage, img, fromCache, usedThumb] { finishImage(img, fromCache, usedThumb); }, Qt::QueuedConnection);
            return;
        }
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, cid, key, thumb, decodeAsync] {
            if (!self || self->m_closed) return;
            if (!cid.startsWith(QLatin1String("local-")) && self->isRemote()) {
                self->fetch(cid, thumb, decodeAsync);
            } else {
                self->m_loading.remove(key);
                self->m_failedImages.insert(key);
            }
        }, Qt::QueuedConnection);
    });
    return {};
}

void Session::fileData(const QString &id, std::function<void(bool, QByteArray)> done) {
    const QString cid = m_files ? m_files->canonicalId(id) : id;
    QByteArray data;
    if (id.startsWith(QLatin1String("local-")) && m_files->readUpload(id, &data)) {
        QTimer::singleShot(0, this, [done, data] { done(true, data); });
    } else if (m_files->readFile(cid, &data, false)) {
        QTimer::singleShot(0, this, [done, data] { done(true, data); });
    } else if (isRemote() && !cid.startsWith(QLatin1String("local-"))) {
        fetch(cid, false, std::move(done));
    } else {
        QTimer::singleShot(0, this, [done] { done(false, {}); });
    }
}

void Session::fetch(const QString &id, bool thumb, std::function<void(bool, QByteArray)> done) {
    const QString key = id + (thumb ? QStringLiteral("#t") : QString());
    auto &waiters = m_waiting[key];
    waiters << std::move(done);
    if (waiters.size() > 1) return;
    QPointer<Session> self(this);

    auto finishFetch = [self, id, key, thumb](bool ok, const QByteArray &data) {
        if (!self) return;
        // Content is immutable per id, so the copy is kept for offline use.
        if (ok) self->m_files->putFile(id, data, thumb);
        const auto waiting = self->m_waiting.take(key);
        for (const auto &w : waiting) w(ok, data);
    };

    const QString encodedId = QString::fromLatin1(QUrl::toPercentEncoding(id));
    auto fallbackRaw = [self, encodedId, thumb, finishFetch] {
        if (!self) return;
        const QString rawPath = QStringLiteral("/attachments/%1/raw%2")
                                    .arg(encodedId, thumb ? QStringLiteral("?thumb=1") : QString());
        self->m_api->get(rawPath, self, [finishFetch](const ApiReply &r) { finishFetch(r.ok, r.body); }, 0);
    };

    const QString grantPath = QStringLiteral("/attachments/%1/grant?compat=1%2")
                                  .arg(encodedId, thumb ? QStringLiteral("&thumb=1") : QString());
    m_api->get(
        grantPath,
        this,
        [self, finishFetch, fallbackRaw](const ApiReply &r) {
            if (!self) return;
            if (!r.ok) {
                if (r.status == 401 || r.status == 403) {
                    finishFetch(false, {});
                    return;
                }
                fallbackRaw();
                return;
            }
            const QJsonObject obj = r.json.toObject();
            const bool direct = obj.value(QLatin1String("direct")).toBool();
            const QString url = obj.value(QLatin1String("url")).toString();
            const QString fallbackUrl = obj.value(QLatin1String("fallbackUrl")).toString();
            const QString blobId = obj.value(QLatin1String("blobId")).toString();
            const QByteArray bk = QByteArray::fromBase64(obj.value(QLatin1String("key")).toString().toLatin1());
            const qint64 rawChunk = qint64(obj.value(QLatin1String("chunkSize")).toDouble(0));
            const qint64 chunkSize = rawChunk > 0 ? rawChunk : BlobCrypto::kChunkSize;

            if (!direct || url.isEmpty() || blobId.isEmpty() || bk.size() != 32 || chunkSize <= 0) {
                fallbackRaw();
                return;
            }

            auto tryDirect = [self, bk, blobId, chunkSize, finishFetch](const QString &targetUrl, std::function<void()> onFail) {
                if (!self) return;
                self->m_api->getDirect(
                    QUrl(targetUrl),
                    self,
                    [bk, blobId, chunkSize, finishFetch, onFail = std::move(onFail)](const ApiReply &dr) {
                        if (dr.ok) {
                            QByteArray plain;
                            if (BlobCrypto::decryptBlob(bk, blobId, dr.body, &plain, chunkSize)) {
                                finishFetch(true, plain);
                                return;
                            }
                        }
                        onFail();
                    },
                    0);
            };

            if (!fallbackUrl.isEmpty() && fallbackUrl != url) {
                tryDirect(url, [tryDirect, fallbackUrl, fallbackRaw] { tryDirect(fallbackUrl, fallbackRaw); });
            } else {
                tryDirect(url, fallbackRaw);
            }
        },
        30000);
}

void Session::openAttachment(const QString &id) {
    const QString name = m_engine->attachmentName(id);
    QPointer<Session> self(this);
    fileData(id, [self, name, id](bool ok, const QByteArray &data) {
        if (!self || !ok) return;
        // A private temporary copy for the system viewer.
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + QStringLiteral("/ivynote-open/") + id;
        QDir().mkpath(dir);
        QString safe = name.isEmpty() ? id : name;
        safe.replace(u'/', u'_').replace(u'\\', u'_');
        QFile f(dir + u'/' + safe);
        if (!f.open(QIODevice::WriteOnly)) return;
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        f.write(data);
        f.close();
        QDesktopServices::openUrl(QUrl::fromLocalFile(f.fileName()));
    });
}

void Session::saveAttachmentAs(const QString &id, QWidget *parent) {
    const QString name = m_engine->attachmentName(id);
    const QString target = QFileDialog::getSaveFileName(parent, QString(), QDir::home().filePath(name));
    if (target.isEmpty()) return;
    fileData(id, [target](bool ok, const QByteArray &data) {
        if (!ok) return;
        QFile f(target);
        if (f.open(QIODevice::WriteOnly)) f.write(data);
    });
}

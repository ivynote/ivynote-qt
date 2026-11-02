#include "vault/session.h"

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
#include <QImageReader>
#include <QJsonObject>
#include <QNetworkInformation>
#include <QPointer>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

Session::Session(const VaultInfo &info, QObject *parent) : QObject(parent), m_info(info) {
    m_images.setMaxCost(256 * 1024); // KiB of decoded pixels
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
    s->m_files = new VaultFiles(info.dir(), cipher);
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
    if (m_events) m_events->stop();
    if (m_engine) {
        m_engine->stop();
        delete m_engine;
        m_engine = nullptr;
    }
    delete m_transport;
    m_transport = nullptr;
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

QImage Session::decode(const QByteArray &data) const {
    QBuffer buf;
    buf.setData(data);
    buf.open(QIODevice::ReadOnly);
    QImageReader reader(&buf);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    const int limit = 2400;
    if (size.isValid() && (size.width() > limit || size.height() > limit))
        reader.setScaledSize(size.scaled(limit, limit, Qt::KeepAspectRatio));
    return reader.read();
}

QImage Session::image(const QString &id, bool thumb) {
    const QString key = id + (thumb ? QStringLiteral("#t") : QString());
    if (QImage *img = m_images.object(key)) return *img;
    if (m_loading.contains(key) || m_closed) return {};
    m_loading.insert(key);
    QPointer<Session> self(this);
    auto store = [self, key, id](bool ok, const QByteArray &data) {
        if (!self) return;
        self->m_loading.remove(key);
        if (!ok) return;
        const QImage img = self->decode(data);
        if (img.isNull()) return;
        self->m_images.insert(key, new QImage(img), std::max<qsizetype>(1, img.sizeInBytes() / 1024));
        emit self->attachmentReady(id);
    };
    QByteArray data;
    if (id.startsWith(QLatin1String("local-"))) {
        const bool ok = m_files->readUpload(id, &data);
        QTimer::singleShot(0, this, [store, ok, data] { store(ok, data); });
    } else if (m_files->readFile(id, &data, thumb) || (thumb && m_files->readFile(id, &data, false))) {
        QTimer::singleShot(0, this, [store, data] { store(true, data); });
    } else if (isRemote()) {
        fetch(id, thumb, store);
    } else {
        m_loading.remove(key);
    }
    return {};
}

void Session::fileData(const QString &id, std::function<void(bool, QByteArray)> done) {
    QByteArray data;
    if (id.startsWith(QLatin1String("local-"))) {
        const bool ok = m_files->readUpload(id, &data);
        QTimer::singleShot(0, this, [done, ok, data] { done(ok, data); });
    } else if (m_files->readFile(id, &data, false)) {
        QTimer::singleShot(0, this, [done, data] { done(true, data); });
    } else if (isRemote()) {
        fetch(id, false, std::move(done));
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
    const QString path = QStringLiteral("/attachments/%1/raw%2").arg(QString::fromLatin1(QUrl::toPercentEncoding(id)), thumb ? QStringLiteral("?thumb=1") : QString());
    m_api->get(path, this, [self, id, key, thumb](const ApiReply &r) {
        if (!self) return;
        // Content is immutable per id, so the copy is kept for offline use.
        if (r.ok) self->m_files->putFile(id, r.body, thumb);
        const auto waiters = self->m_waiting.take(key);
        for (const auto &w : waiters) w(r.ok, r.body);
    }, 0);
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

#include "ui/updater.h"
#include "core/i18n.h"
#include "ui/officialsites.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QProcess>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <memory>

namespace {
class Download final : public QProgressDialog {
public:
    Download(QWidget *parent, QString path, QString destination, QByteArray digest, qint64 size, bool replace)
        : QProgressDialog(T("正在下载更新"), T("取消"), 0, 100, parent), path(std::move(path)),
          destination(std::move(destination)), digest(std::move(digest)), size(size), replace(replace) {
        setWindowTitle(T("检查更新"));
        setAutoClose(false);
        setAutoReset(false);
        setMinimumDuration(0);
        setAttribute(Qt::WA_DeleteOnClose);
        connect(this, &QProgressDialog::canceled, this, &QObject::deleteLater);
    }
    void start() {
        file = std::make_unique<QSaveFile>(destination);
        if (!file->open(QIODevice::WriteOnly)) return finish(file->errorString());
        hash.reset();
        written = 0;
        const QString site = attempt++ == 0 ? QStringLiteral("https://ivynote.net") : QStringLiteral("https://ivynote.app");
        QNetworkRequest request(QUrl(site + path));
        request.setTransferTimeout(30000);
        auto *reply = network.get(request);
        reply->setReadBufferSize(512 * 1024);
        const auto consume = [this, reply] {
            const QByteArray bytes = reply->readAll();
            if (written + bytes.size() > size || file->write(bytes) != bytes.size()) {
                reply->abort();
                return;
            }
            hash.addData(bytes);
            written += bytes.size();
            setValue(int(100.0 * written / size));
        };
        connect(reply, &QIODevice::readyRead, this, consume);
        connect(reply, &QNetworkReply::finished, this, [this, reply, consume] {
            reply->deleteLater();
            if (wasCanceled()) return;
            if (reply->bytesAvailable()) consume();
            if (reply->error() != QNetworkReply::NoError || written != size || hash.result().toHex() != digest) {
                file.reset();
                if (attempt < 2) return start();
                return finish(T("更新下载或校验失败"));
            }
            if (!file->setPermissions(replace ? QFileInfo(destination).permissions() : QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner) || !file->commit())
                return finish(file->errorString());
            if (replace) return finish(T("更新已安装，重新启动后生效"));
#ifdef Q_OS_WIN
            if (!QProcess::startDetached(destination, {})) return finish(T("无法启动安装程序"));
#else
            if (!QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(destination).absolutePath()))) return finish(destination);
#endif
            finish(T("安装包已下载") + QStringLiteral("\n") + destination);
        });
        show();
    }
private:
    void finish(const QString &message) {
        hide();
        QMessageBox::information(parentWidget(), T("检查更新"), message);
        deleteLater();
    }
    QString path, destination;
    QByteArray digest;
    qint64 size, written = 0;
    bool replace;
    int attempt = 0;
    QNetworkAccessManager network;
    QCryptographicHash hash{QCryptographicHash::Sha256};
    std::unique_ptr<QSaveFile> file;
};

class CheckSession final : public QObject {
public:
    explicit CheckSession(QWidget *window)
        : QObject(window), window(window), manager(new QNetworkAccessManager(this)) {
        setObjectName(QStringLiteral("updateCheckSession"));
    }

    void ensureProgress() {
        if (!window || finished) return;
        if (!progress) {
            progress = new QProgressDialog(T("正在检查更新…"), T("取消"), 0, 0, window);
            progress->setWindowTitle(T("检查更新"));
            progress->setObjectName(QStringLiteral("updateCheckProgress"));
            progress->setAutoClose(false);
            progress->setAutoReset(false);
            progress->setMinimumDuration(0);
            progress->setAttribute(Qt::WA_DeleteOnClose);
            connect(progress, &QProgressDialog::canceled, this, [this] { cancel(); });
        }
        progress->show();
        progress->raise();
        progress->activateWindow();
    }

    void dismissProgress() {
        if (!progress) return;
        progress->disconnect(this);
        progress->hide();
        progress->deleteLater();
        progress = nullptr;
    }

    void notify(updater::Outcome outcome, const QString &message) {
        auto list = std::move(callbacks);
        callbacks.clear();
        for (const auto &cb : list) {
            if (cb) cb(outcome, message);
        }
    }

    void complete() {
        finished = true;
        setObjectName(QString());
        dismissProgress();
    }

    void cancel() {
        if (finished) return;
        complete();
        if (manager) {
            delete manager;
            manager = nullptr;
        }
        notify(updater::Outcome::Canceled, QString());
        deleteLater();
    }

    QPointer<QWidget> window;
    QPointer<QNetworkAccessManager> manager;
    QPointer<QProgressDialog> progress;
    QList<updater::Callback> callbacks;
    bool manual = false;
    bool showDialog = false;
    bool finished = false;
};
}

void updater::check(QWidget *window, bool manual, Callback onFinished) {
    if (!window) return;
    if (auto *download = window->findChild<QProgressDialog *>(QStringLiteral("updateDownload"), Qt::FindDirectChildrenOnly)) {
        if (manual) {
            download->show();
            download->raise();
            download->activateWindow();
        }
        if (onFinished) onFinished(Outcome::UpdateAvailable, T("正在下载更新"));
        return;
    }
    if (auto *obj = window->findChild<QObject *>(QStringLiteral("updateCheckSession"), Qt::FindDirectChildrenOnly)) {
        auto *existing = static_cast<CheckSession *>(obj);
        if (manual) existing->manual = true;
        if (onFinished) {
            existing->callbacks.append(std::move(onFinished));
        } else if (manual) {
            existing->showDialog = true;
            existing->ensureProgress();
        }
        return;
    }

#if defined(Q_OS_WIN)
    const QString platform = QStringLiteral("windows"), kind = QStringLiteral("exe");
#elif defined(Q_OS_MACOS)
    const QString platform = QStringLiteral("macos"), kind = QStringLiteral("dmg");
#else
    const QString platform = QStringLiteral("linux"), kind = QStringLiteral("appimage");
#endif
    const QString machine = QSysInfo::buildCpuArchitecture();
    const QString arch = machine == QStringLiteral("x86_64") ? QStringLiteral("x64") : machine == QStringLiteral("arm64") ? QStringLiteral("arm64") : machine;
    const QString wantedArch = arch == QStringLiteral("aarch64") ? QStringLiteral("arm64") : arch;
    const QString path = QStringLiteral("/api/v1/update/qt?version=%1&platform=%2&arch=%3&kind=%4").arg(QStringLiteral(IVY_VERSION), platform, wantedArch, kind);

    auto *session = new CheckSession(window);
    session->manual = manual;
    if (onFinished) {
        session->callbacks.append(std::move(onFinished));
    } else if (manual) {
        session->showDialog = true;
        session->ensureProgress();
    }

    auto promptParent = [window]() -> QWidget * {
        if (QWidget *modal = QApplication::activeModalWidget()) return modal;
        return window;
    };

    official::fetch(session->manager, session, path, [session, window, platform, kind, wantedArch, promptParent](const QJsonObject &response) {
        if (session->finished) return true;
        if (!response.value("update_available").isBool() || response.value("latest").toString().isEmpty()) return false;
        if (!response.value("update_available").toBool()) {
            session->complete();
            const QString msg = T("当前已是最新版本");
            session->notify(Outcome::UpToDate, msg);
            if (session->showDialog) QMessageBox::information(promptParent(), T("检查更新"), msg);
            session->deleteLater();
            return true;
        }
        const auto asset = response.value("download").toObject();
        const QUrl url(asset.value("url").toString());
        const QString name = asset.value("name").toString();
        const QString tag = response.value("tag").toString();
        static const QRegularExpression safeName(QStringLiteral("^[a-zA-Z0-9][a-zA-Z0-9._+-]*$"));
        static const QRegularExpression sha(QStringLiteral("^[a-fA-F0-9]{64}$"));
        const QString digest = asset.value("sha256").toString();
        const qint64 size = asset.value("size").toInteger();
        if (!safeName.match(name).hasMatch() || !safeName.match(tag).hasMatch() || !sha.match(digest).hasMatch() ||
            size <= 0 || size > (qint64(4) << 30) || asset.value("platform").toString() != platform || asset.value("kind").toString() != kind ||
            (asset.value("arch").toString() != wantedArch && asset.value("arch").toString() != QStringLiteral("universal")) ||
            url.path() != QStringLiteral("/dl/qt/") + tag + u'/' + name) return false;

        session->complete();
        const QString latest = response.value("latest").toString();
        session->notify(Outcome::UpdateAvailable, T("发现新版本 %1").arg(latest));
        QWidget *parent = promptParent();
        if (QMessageBox::question(parent, T("检查更新"), T("下载并安装 %1？").arg(latest)) == QMessageBox::Yes) {
            if (auto *modal = qobject_cast<QDialog *>(parent); modal && modal != window) modal->accept();
            const QString appImage = qEnvironmentVariable("APPIMAGE");
            const bool replace = platform == QStringLiteral("linux") && QFileInfo(appImage).isAbsolute() && QFileInfo(appImage).isFile();
            QDir cache(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
            if (!replace && !cache.mkpath(QStringLiteral("updates"))) {
                QMessageBox::warning(window, T("检查更新"), T("无法创建下载目录"));
            } else {
                auto *job = new Download(window, url.path(), replace ? appImage : cache.filePath(QStringLiteral("updates/") + name), digest.toLatin1().toLower(), size, replace);
                job->setObjectName(QStringLiteral("updateDownload"));
                job->start();
            }
        }
        session->deleteLater();
        return true;
    }, [session, promptParent] {
        if (session->finished) return;
        session->complete();
        const QString msg = T("没有找到更新的版本，请稍后再试");
        session->notify(Outcome::Unavailable, msg);
        if (session->showDialog) QMessageBox::information(promptParent(), T("检查更新"), msg);
        session->deleteLater();
    });
}

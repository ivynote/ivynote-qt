#include "core/i18n.h"
#include "storage/secrets.h"
#include "ui/icons.h"
#include "ui/mainwindow.h"
#include "ui/settingsdialog.h"
#include "ui/theme.h"
#include "vault/vaults.h"

#include <QApplication>
#include <QDir>
#include <QLockFile>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTimer>
#include <sodium.h>

int main(int argc, char *argv[]) {
    QApplication::setOrganizationName(QStringLiteral("Ivynote"));
    QApplication::setOrganizationDomain(QStringLiteral("ivynote.app"));
    QApplication::setApplicationName(QStringLiteral("Ivynote"));
    QApplication::setApplicationVersion(QStringLiteral(IVY_VERSION));
    QApplication::setDesktopFileName(QStringLiteral("app.ivynote.Ivynote"));
    QApplication app(argc, argv);
    app.setWindowIcon(icons::appIcon());

    if (sodium_init() < 0) {
        QMessageBox::critical(nullptr, QStringLiteral("Ivynote"), QStringLiteral("libsodium failed to initialize."));
        return 1;
    }
    // Development aids: keep secrets out of the system keychain, and save a
    // screenshot of the window after a moment, then quit.
    if (qEnvironmentVariableIsSet("IVY_SECRETS_IN_FILE")) secrets::setFallbackOnly(true);
    i18n::setLang(i18n::Lang(Prefs::instance()->lang()));
    Theme::setReducedMotion(Prefs::instance()->reduceMotion());
    Theme::instance()->setMode(Theme::Mode(Prefs::instance()->theme()));

    // One instance at a time: two engines on one vault would race each other.
    QDir().mkpath(vaults::dataRoot());
    QLockFile lock(vaults::dataRoot() + QStringLiteral("/ivynote.lock"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(200)) {
        QMessageBox::information(nullptr, T("常青笔记"), T("常青笔记已经在运行。"));
        return 0;
    }

    MainWindow w;
    w.show();
    w.start();
    if (const QString shot = qEnvironmentVariable("IVY_SCREENSHOT"); !shot.isEmpty()) {
        QTimer::singleShot(qEnvironmentVariableIntValue("IVY_SCREENSHOT_DELAY") ?: 2500, &app, [&w, shot] {
            w.grab().save(shot);
            QApplication::quit();
        });
    }
    return app.exec();
}

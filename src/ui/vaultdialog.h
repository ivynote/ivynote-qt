// Creating a local vault or connecting one to an Ivynote server, and signing
// in again when a server session has ended.
#pragma once

#include "core/models.h"
#include "vault/vaults.h"

#include <QDialog>

class ApiClient;
class QLabel;
class QLineEdit;
class QCheckBox;
class QStackedWidget;
class QTabBar;
class PillButton;

class VaultDialog : public QDialog {
    Q_OBJECT
public:
    explicit VaultDialog(QWidget *parent = nullptr, bool startWithServer = false);
    // The vault to open: a new one, or an existing one for the same account.
    QString vaultId() const { return m_vaultId; }

private:
    enum Page { Choose, Local, Server, Account };
    void go(Page p);
    void createLocal();
    void checkServer();
    void submitAccount();
    void setBusy(bool busy);
    void showError(QLabel *label, const QString &text);

    QStackedWidget *m_pages;
    QString m_vaultId;
    // Local
    QLineEdit *m_localName;
    QCheckBox *m_localEncrypt;
    QLabel *m_localError;
    // Server
    QLineEdit *m_server;
    QLabel *m_serverError;
    QLabel *m_serverWarn;
    PillButton *m_serverNext;
    ApiClient *m_api = nullptr;
    ServerInfo m_info;
    // Account
    QLabel *m_siteTitle;
    QLabel *m_siteNote;
    QTabBar *m_mode;
    QLineEdit *m_identity, *m_email, *m_displayName, *m_password;
    QCheckBox *m_remoteEncrypt;
    QLabel *m_accountError;
    PillButton *m_submit;
};

// Signs in again to a vault's server. Only the vault's own account is accepted.
class SignInDialog : public QDialog {
    Q_OBJECT
public:
    SignInDialog(const VaultInfo &vault, QWidget *parent = nullptr);
    QString token() const { return m_token; }
    User user() const { return m_user; }

private:
    void submit();

    VaultInfo m_vault;
    ApiClient *m_api;
    QLineEdit *m_identity, *m_password;
    QLabel *m_error;
    PillButton *m_submit;
    QString m_token;
    User m_user;
};

QString authErrorText(int status, const QString &error);

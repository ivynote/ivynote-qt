#include "ui/vaultdialog.h"

#include "core/i18n.h"
#include "storage/secrets.h"
#include "sync/api.h"
#include "ui/dialogs.h"
#include "ui/icons.h"
#include "ui/officialsites.h"
#include "ui/theme.h"
#include "ui/widgets.h"
#include "storage/crypto.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QTabBar>
#include <QTimeZone>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace {

QLabel *title(const QString &text, QWidget *parent) {
    auto *l = new QLabel(text, parent);
    l->setFont(Theme::serif(22));
    l->setAlignment(Qt::AlignHCenter);
    return l;
}

QLabel *errorLabel(QWidget *parent) {
    auto *l = new QLabel(parent);
    l->setWordWrap(true);
    QPalette p = l->palette();
    p.setColor(QPalette::WindowText, Theme::t().danger);
    l->setPalette(p);
    l->hide();
    return l;
}

QLineEdit *field(const QString &placeholder, QWidget *parent) {
    auto *e = new QLineEdit(parent);
    e->setPlaceholderText(placeholder);
    e->setMinimumHeight(36);
    e->setClearButtonEnabled(false);
    return e;
}

// A large option button: icon, title and one line of explanation.
class OptionCard : public QToolButton {
public:
    OptionCard(const QString &icon, const QString &head, const QString &text, QWidget *parent)
        : QToolButton(parent), m_icon(icon), m_head(head), m_text(text) {
        setCursor(Qt::PointingHandCursor);
        setMinimumHeight(84);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setAttribute(Qt::WA_Hover);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const Tokens &k = Theme::t();
        const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
        p.setPen(Qt::NoPen);
        p.setBrush(underMouse() ? k.hover : k.sunken);
        p.drawRoundedRect(r, Theme::radius, Theme::radius);
        if (hasFocus()) {
            p.setPen(QPen(Theme::alpha(k.ivy, 0.45), 2));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(r.adjusted(1, 1, -1, -1), Theme::radius - 1, Theme::radius - 1);
        }
        p.drawPixmap(QRect(20, int(r.center().y()) - 12, 24, 24), icons::pixmap(m_icon, k.ivy, 24, devicePixelRatioF()));
        p.setPen(k.ink);
        p.setFont(Theme::uiFont(15, QFont::DemiBold));
        p.drawText(QRect(60, 16, width() - 76, 24), Qt::AlignLeft | Qt::AlignVCenter, m_head);
        p.setPen(k.muted);
        p.setFont(Theme::uiFont(12.5));
        p.drawText(QRect(60, 40, width() - 76, height() - 48), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, m_text);
    }

private:
    QString m_icon, m_head, m_text;
};

} // namespace

QString authErrorText(int status, const QString &error) {
    if (status == 0) return T("连接不上服务器，请检查地址和网络。");
    if (status == 401) return T("用户名或密码不正确。");
    if (status == 429) return T("尝试次数过多，请 15 分钟后再试。");
    if (status == 403 && error.contains(QLatin1String("regist"), Qt::CaseInsensitive)) return T("这台服务器已关闭注册。");
    if (status == 403) return T("账户已被停用，请联系管理员。");
    if (status == 409) return T("用户名或邮箱已被使用。");
    if (status >= 500) return T("服务器暂时不可用，请稍后再试。");
    return error.isEmpty() ? T("出错了") : error;
}

VaultDialog::VaultDialog(QWidget *parent, bool startWithServer) : QDialog(parent) {
    setWindowTitle(T("新建或接入笔记库"));
    setMinimumWidth(480);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(28, 24, 28, 24);
    m_pages = new QStackedWidget(this);
    outer->addWidget(m_pages);

    // Choose.
    {
        auto *page = new QWidget;
        auto *col = new QVBoxLayout(page);
        col->setSpacing(12);
        auto *leaf = new LeafWidget(56, 1.0, page);
        col->addWidget(leaf, 0, Qt::AlignHCenter);
        col->addWidget(title(T("新建或接入笔记库"), page));
        auto *sub = mutedLabel(T("每个笔记库独立保存。本地笔记库以后不能改为同步，但可以把笔记导入到同步笔记库。"), page, 12.5);
        sub->setWordWrap(true);
        sub->setAlignment(Qt::AlignHCenter);
        col->addWidget(sub);
        col->addSpacing(8);
        auto *local = new OptionCard(QStringLiteral("database"), T("新建本地笔记库"), T("只保存在这台电脑上，不需要服务器，打开即可使用。"), page);
        local->setObjectName(QStringLiteral("cardCreateLocal"));
        auto *existing = new OptionCard(QStringLiteral("folder"), T("打开已有本地笔记库"), T("选择之前保存过的笔记库文件夹，或自动扫描找回。"), page);
        existing->setObjectName(QStringLiteral("cardOpenExisting"));
        auto *server = new OptionCard(QStringLiteral("cloud"), T("连接 Ivynote 服务器"), T("登录自托管的 Ivynote，在多台设备之间可靠同步。"), page);
        server->setObjectName(QStringLiteral("cardConnectServer"));
        connect(local, &QToolButton::clicked, this, [this] { go(Local); });
        connect(existing, &QToolButton::clicked, this, [this] { go(OpenExisting); });
        connect(server, &QToolButton::clicked, this, [this] { go(Server); });
        col->addWidget(local);
        col->addWidget(existing);
        col->addWidget(server);
        col->addStretch(1);
        m_pages->addWidget(page);
    }
    // Local.
    {
        auto *page = new QWidget;
        auto *col = new QVBoxLayout(page);
        col->setSpacing(10);
        col->addWidget(title(T("新建本地笔记库"), page));
        col->addSpacing(6);
        m_localName = field(T("笔记库名称"), page);
        m_localName->setObjectName(QStringLiteral("localNameEdit"));
        m_localName->setText(T("我的笔记"));
        col->addWidget(m_localName);
        m_localEncrypt = new QCheckBox(T("加密本机数据"), page);
        col->addWidget(m_localEncrypt);
        auto *hint = mutedLabel(T("密钥保存在系统钥匙串中，并会给你一把恢复密钥。丢失两者将无法打开笔记库。"), page, 12);
        hint->setWordWrap(true);
        col->addWidget(hint);

        // Collapsible advanced configuration (collapsed by default so normal creation stays effortless).
        m_localAdvToggle = new QToolButton(page);
        m_localAdvToggle->setObjectName(QStringLiteral("localAdvancedToggle"));
        m_localAdvToggle->setCheckable(true);
        m_localAdvToggle->setChecked(false);
        m_localAdvToggle->setAutoRaise(true);
        m_localAdvToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        m_localAdvToggle->setIconSize(QSize(14, 14));
        m_localAdvToggle->setIcon(icons::themed(QStringLiteral("chevron-right")));
        m_localAdvToggle->setText(T("高级设置"));
        m_localAdvToggle->setCursor(Qt::PointingHandCursor);
        col->addWidget(m_localAdvToggle, 0, Qt::AlignLeft);

        m_localAdvBox = new QWidget(page);
        m_localAdvBox->setObjectName(QStringLiteral("localAdvancedBox"));
        auto *advCol = new QVBoxLayout(m_localAdvBox);
        advCol->setContentsMargins(12, 4, 0, 4);
        advCol->setSpacing(6);
        advCol->addWidget(new QLabel(T("笔记库位置"), m_localAdvBox));
        auto *pathRow = new QHBoxLayout;
        pathRow->setSpacing(6);
        m_localPath = new QLineEdit(m_localAdvBox);
        m_localPath->setObjectName(QStringLiteral("localPathEdit"));
        m_localPath->setReadOnly(true);
        m_localPath->setPlaceholderText(T("默认位置（自动管理）"));
        auto *browseBtn = new QPushButton(T("选择位置…"), m_localAdvBox);
        browseBtn->setObjectName(QStringLiteral("localBrowseBtn"));
        m_localResetPath = new QPushButton(T("恢复默认"), m_localAdvBox);
        m_localResetPath->setObjectName(QStringLiteral("localResetPathBtn"));
        m_localResetPath->setEnabled(false);
        pathRow->addWidget(m_localPath, 1);
        pathRow->addWidget(browseBtn);
        pathRow->addWidget(m_localResetPath);
        advCol->addLayout(pathRow);
        m_localPathHint = mutedLabel(QString(), m_localAdvBox, 12);
        m_localPathHint->setWordWrap(true);
        m_localPathHint->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        advCol->addWidget(m_localPathHint);
        m_localAdvBox->hide();
        col->addWidget(m_localAdvBox);
        updateLocalPathUi();

        connect(m_localAdvToggle, &QToolButton::toggled, this, [this](bool expanded) {
            m_localAdvBox->setVisible(expanded);
            m_localAdvToggle->setIcon(icons::themed(expanded ? QStringLiteral("chevron-down") : QStringLiteral("chevron-right")));
            adjustSize();
        });
        connect(browseBtn, &QPushButton::clicked, this, [this] {
            const QString startDir = m_localCustomPath.isEmpty() ? QDir::homePath() : m_localCustomPath;
            const QString dir = QFileDialog::getExistingDirectory(this, T("选择笔记库位置"), startDir);
            if (dir.isEmpty()) return;
            m_localCustomPath = QDir::cleanPath(dir);
            m_localError->hide();
            updateLocalPathUi();
        });
        connect(m_localResetPath, &QPushButton::clicked, this, [this] {
            m_localCustomPath.clear();
            m_localError->hide();
            updateLocalPathUi();
        });

        m_localError = errorLabel(page);
        col->addWidget(m_localError);
        col->addStretch(1);
        auto *row = new QHBoxLayout;
        auto *back = new PillButton(T("返回"), PillButton::Ghost, page);
        auto *create = new PillButton(T("创建"), PillButton::Primary, page);
        connect(back, &QToolButton::clicked, this, [this] { go(Choose); });
        connect(create, &QToolButton::clicked, this, &VaultDialog::createLocal);
        connect(m_localName, &QLineEdit::returnPressed, this, &VaultDialog::createLocal);
        row->addWidget(back);
        row->addStretch(1);
        row->addWidget(create);
        col->addLayout(row);
        m_pages->addWidget(page);
    }
    // OpenExisting.
    {
        auto *page = new QWidget;
        auto *col = new QVBoxLayout(page);
        col->setSpacing(10);
        col->addWidget(title(T("打开已有本地笔记库"), page));
        auto *sub = mutedLabel(T("选择之前保存过的笔记库文件夹，或自动扫描本机已有的 Ivynote 笔记库。"), page, 12.5);
        sub->setWordWrap(true);
        sub->setAlignment(Qt::AlignHCenter);
        col->addWidget(sub);
        col->addSpacing(4);

        auto *pickRow = new QHBoxLayout;
        pickRow->setSpacing(6);
        m_existingPath = field(T("选择或输入笔记库文件夹路径"), page);
        m_existingPath->setObjectName(QStringLiteral("existingPathEdit"));
        auto *browse = new QPushButton(T("选择文件夹…"), page);
        browse->setObjectName(QStringLiteral("existingBrowseBtn"));
        m_existingScan = new QPushButton(T("自动扫描"), page);
        m_existingScan->setObjectName(QStringLiteral("existingScanBtn"));
        pickRow->addWidget(m_existingPath, 1);
        pickRow->addWidget(browse);
        pickRow->addWidget(m_existingScan);
        col->addLayout(pickRow);

        m_existingStatus = mutedLabel(QString(), page, 12);
        m_existingStatus->setObjectName(QStringLiteral("existingStatusLabel"));
        m_existingStatus->setWordWrap(true);
        m_existingStatus->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        m_existingStatus->hide();
        col->addWidget(m_existingStatus);

        m_scanStatus = mutedLabel(T("点击「自动扫描」查找本机已有的本地笔记库。"), page, 12);
        m_scanStatus->setObjectName(QStringLiteral("scanStatusLabel"));
        m_scanStatus->setWordWrap(true);
        col->addWidget(m_scanStatus);

        m_discoveredList = new QListWidget(page);
        m_discoveredList->setObjectName(QStringLiteral("discoveredVaultsList"));
        m_discoveredList->setMinimumHeight(140);
        m_discoveredList->setMaximumHeight(220);
        m_discoveredList->setWordWrap(true);
        col->addWidget(m_discoveredList);

        m_existingError = errorLabel(page);
        col->addWidget(m_existingError);

        auto *row = new QHBoxLayout;
        auto *back = new PillButton(T("返回"), PillButton::Ghost, page);
        m_openExisting = new PillButton(T("打开笔记库"), PillButton::Primary, page);
        m_openExisting->setObjectName(QStringLiteral("openExistingBtn"));
        m_openExisting->setEnabled(false);
        connect(back, &QToolButton::clicked, this, [this] { go(Choose); });
        connect(m_openExisting, &QToolButton::clicked, this, &VaultDialog::openExisting);
        connect(m_existingPath, &QLineEdit::textChanged, this, [this] { verifyExistingPath(); });
        connect(m_existingPath, &QLineEdit::returnPressed, this, [this] {
            if (m_existingValid) openExisting();
        });
        connect(browse, &QPushButton::clicked, this, [this] {
            const QString startDir = m_existingPath->text().trimmed().isEmpty() ? QDir::homePath() : m_existingPath->text().trimmed();
            const QString dir = QFileDialog::getExistingDirectory(this, T("选择已有笔记库文件夹"), startDir);
            if (!dir.isEmpty()) m_existingPath->setText(QDir::toNativeSeparators(QDir::cleanPath(dir)));
        });
        connect(m_existingScan, &QPushButton::clicked, this, &VaultDialog::startScan);
        connect(m_discoveredList, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
            if (!item) return;
            const QString dir = item->data(Qt::UserRole).toString();
            if (!dir.isEmpty()) m_existingPath->setText(QDir::toNativeSeparators(dir));
        });
        connect(m_discoveredList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
            if (!item) return;
            const QString dir = item->data(Qt::UserRole).toString();
            if (dir.isEmpty()) return;
            m_existingPath->setText(QDir::toNativeSeparators(dir));
            if (m_existingValid) openExisting();
        });

        m_scanWatcher = new QFutureWatcher<QList<DiscoveredVault>>(this);
        connect(m_scanWatcher, &QFutureWatcher<QList<DiscoveredVault>>::finished, this, [this] {
            m_existingScan->setEnabled(true);
            applyScanResults(m_scanWatcher->result());
        });

        row->addWidget(back);
        row->addStretch(1);
        row->addWidget(m_openExisting);
        col->addLayout(row);
        m_pages->addWidget(page);
    }
    // Server address.
    {
        auto *page = new QWidget;
        auto *col = new QVBoxLayout(page);
        col->setSpacing(10);
        col->addWidget(title(T("连接服务器"), page));
        auto *sub = mutedLabel(T("输入你的 Ivynote 服务器地址"), page, 12.5);
        sub->setAlignment(Qt::AlignHCenter);
        col->addWidget(sub);
        col->addSpacing(6);
        m_server = field(QStringLiteral("notes.example.com"), page);
        auto *nodes = new QComboBox(page);
        nodes->addItems({T("中国大陆"), T("全球"), T("自定义服务器")});
        col->addWidget(nodes);
        col->addWidget(m_server);
        const QStringList mainlandZones{QStringLiteral("Asia/Shanghai"), QStringLiteral("Asia/Urumqi"),
            QStringLiteral("Asia/Chongqing"), QStringLiteral("Asia/Chungking"), QStringLiteral("Asia/Harbin"),
            QStringLiteral("Asia/Kashgar"), QStringLiteral("PRC")};
        nodes->setCurrentIndex(mainlandZones.contains(QString::fromUtf8(QTimeZone::systemTimeZoneId())) ? 0 : 1);
        const auto selectNode = [this, nodes](int index) {
            if (index == 2) {
                m_server->clear();
                m_server->setFocus();
                return;
            }
            const QString id = index == 0 ? QStringLiteral("cn") : QStringLiteral("global");
            const QString fallback = index == 0 ? QStringLiteral("https://web.ivynote.net") : QStringLiteral("https://web.ivynote.app");
            m_server->setText(QSettings().value(QStringLiteral("officialNodes/") + id, fallback).toString());
        };
        connect(nodes, &QComboBox::activated, this, selectNode);
        connect(m_server, &QLineEdit::textEdited, nodes, [nodes] { nodes->setCurrentIndex(2); });
        selectNode(nodes->currentIndex());
        official::fetch(new QNetworkAccessManager(this), this, QStringLiteral("/api/v1/regions"),
            [this, nodes, selectNode](const QJsonObject &response) {
                QHash<QString, QString> addresses;
                for (const auto &region : response.value(QStringLiteral("regions")).toArray()) {
                    const auto object = region.toObject();
                    const auto id = object.value(QStringLiteral("id")).toString();
                    const QUrl url(object.value(QStringLiteral("url")).toString(), QUrl::StrictMode);
                    if (url.isValid() && url.scheme() == QStringLiteral("https") && !url.host().isEmpty() &&
                        url.userInfo().isEmpty() && !url.authority().contains(u'@') && !url.hasQuery() &&
                        !url.hasFragment() && (url.path().isEmpty() || url.path() == QStringLiteral("/"))) {
                        addresses.insert(id, url.toString(QUrl::RemovePath | QUrl::FullyEncoded));
                    }
                }
                if (!addresses.contains(QStringLiteral("cn")) || !addresses.contains(QStringLiteral("global"))) return false;
                QSettings settings;
                for (const auto &id : {QStringLiteral("cn"), QStringLiteral("global")})
                    settings.setValue(QStringLiteral("officialNodes/") + id, addresses.value(id));
                if (!m_api && nodes->currentIndex() < 2) selectNode(nodes->currentIndex());
                return true;
            });
        m_serverWarn = new QLabel(page);
        m_serverWarn->setWordWrap(true);
        QPalette wp = m_serverWarn->palette();
        wp.setColor(QPalette::WindowText, Theme::t().warnInk);
        m_serverWarn->setPalette(wp);
        m_serverWarn->hide();
        col->addWidget(m_serverWarn);
        m_serverError = errorLabel(page);
        col->addWidget(m_serverError);
        col->addStretch(1);
        auto *row = new QHBoxLayout;
        auto *back = new PillButton(T("返回"), PillButton::Ghost, page);
        m_serverNext = new PillButton(T("继续"), PillButton::Primary, page);
        connect(back, &QToolButton::clicked, this, [this] { go(Choose); });
        connect(m_serverNext, &QToolButton::clicked, this, &VaultDialog::checkServer);
        connect(m_server, &QLineEdit::returnPressed, this, &VaultDialog::checkServer);
        connect(m_server, &QLineEdit::textChanged, this, [this](const QString &t) {
            const QUrl url = ApiClient::normalizeServer(t);
            const bool insecure = url.scheme() == QLatin1String("http") && !ApiClient::isLocalHost(url);
            m_serverWarn->setText(T("⚠ 这个地址使用未加密的 http://，密码和笔记会以明文传输。除非在可信的内网，请使用 https://。"));
            m_serverWarn->setVisible(insecure);
            m_serverError->hide();
        });
        row->addWidget(back);
        row->addStretch(1);
        row->addWidget(m_serverNext);
        col->addLayout(row);
        m_pages->addWidget(page);
    }
    // Account.
    {
        auto *page = new QWidget;
        auto *col = new QVBoxLayout(page);
        col->setSpacing(10);
        m_siteTitle = title(QString(), page);
        col->addWidget(m_siteTitle);
        m_siteNote = mutedLabel(QString(), page, 12.5);
        m_siteNote->setAlignment(Qt::AlignHCenter);
        m_siteNote->setWordWrap(true);
        col->addWidget(m_siteNote);
        m_mode = new QTabBar(page);
        m_mode->addTab(T("登录"));
        m_mode->addTab(T("注册"));
        m_mode->setExpanding(true);
        m_mode->setDrawBase(false);
        col->addWidget(m_mode);
        m_identity = field(T("用户名或邮箱"), page);
        m_email = field(T("邮箱（可选）"), page);
        m_displayName = field(T("显示名称（可选）"), page);
        m_password = field(T("密码"), page);
        m_password->setEchoMode(QLineEdit::Password);
        col->addWidget(m_identity);
        col->addWidget(m_email);
        col->addWidget(m_displayName);
        col->addWidget(m_password);
        m_remoteEncrypt = new QCheckBox(T("加密本机数据（推荐）"), page);
        m_remoteEncrypt->setChecked(true);
        m_remoteEncrypt->setToolTip(T("服务器上的数据始终由服务器加密；这里加密的是这台电脑上的副本。"));
        col->addWidget(m_remoteEncrypt);
        m_accountError = errorLabel(page);
        col->addWidget(m_accountError);
        col->addStretch(1);
        auto *row = new QHBoxLayout;
        auto *back = new PillButton(T("返回"), PillButton::Ghost, page);
        m_submit = new PillButton(T("登录"), PillButton::Primary, page);
        connect(back, &QToolButton::clicked, this, [this] { go(Server); });
        connect(m_submit, &QToolButton::clicked, this, &VaultDialog::submitAccount);
        connect(m_password, &QLineEdit::returnPressed, this, &VaultDialog::submitAccount);
        connect(m_mode, &QTabBar::currentChanged, this, [this](int i) {
            const bool reg = i == 1;
            m_identity->setPlaceholderText(reg ? T("用户名") : T("用户名或邮箱"));
            m_email->setVisible(reg);
            m_displayName->setVisible(reg);
            m_submit->setText(reg ? T("注册") : T("登录"));
            m_submit->updateGeometry();
            m_accountError->hide();
        });
        row->addWidget(back);
        row->addStretch(1);
        row->addWidget(m_submit);
        col->addLayout(row);
        m_pages->addWidget(page);
    }
    go(startWithServer ? Server : Choose);
}

void VaultDialog::showLocalCreate() { go(Local); }

void VaultDialog::showOpenExisting() { go(OpenExisting); }

void VaultDialog::go(Page p) {
    m_pages->setCurrentIndex(p);
    rise(m_pages->currentWidget(), 260);
    if (p == Local) m_localName->setFocus(), m_localName->selectAll();
    if (p == OpenExisting) {
        m_existingPath->setFocus();
        if (!m_scannedOnce) {
            m_scannedOnce = true;
            startScan();
        }
    }
    if (p == Server) m_server->setFocus();
    adjustSize();
}

void VaultDialog::showError(QLabel *label, const QString &text) {
    label->setText(text);
    label->setVisible(!text.isEmpty());
}

void VaultDialog::setBusy(bool busy) {
    m_serverNext->setEnabled(!busy);
    m_submit->setEnabled(!busy);
    setCursor(busy ? Qt::BusyCursor : Qt::ArrowCursor);
}

void VaultDialog::updateLocalPathUi() {
    const bool custom = !m_localCustomPath.isEmpty();
    m_localPath->setText(custom ? QDir::toNativeSeparators(m_localCustomPath) : QString());
    m_localResetPath->setEnabled(custom);
    m_localPathHint->setText(custom
        ? T("所选文件夹将直接作为笔记库目录（不会在其中再创建子文件夹）。")
        : T("默认保存在应用数据目录中。如需存放到其他位置，请点击「选择位置…」。"));
}

void VaultDialog::createLocal() {
    const QString name = m_localName->text().trimmed();
    if (name.isEmpty()) return showError(m_localError, T("请填写名称。"));
    showError(m_localError, QString());
    VaultInfo v;
    v.name = name.left(60);
    v.kind = VaultInfo::Local;
    v.encrypted = m_localEncrypt->isChecked();
    v.path = m_localCustomPath;
    QString err;
    if (!vaults::create(v, &err)) return showError(m_localError, err);
    if (v.encrypted) {
        QByteArray key;
        secrets::get(v.vaultKeyKey(), &key);
        dialogs::showRecoveryKey(this, v.name, VaultCipher::encodeRecoveryKey(key), true);
    }
    m_vaultId = v.id;
    accept();
}

void VaultDialog::verifyExistingPath() {
    showError(m_existingError, QString());
    const QString raw = m_existingPath->text().trimmed();
    if (raw.isEmpty()) {
        m_existingValid = false;
        m_openExisting->setEnabled(false);
        m_existingStatus->hide();
        return;
    }
    const DiscoveredVault d = vaults::inspectDir(raw);
    QPalette pal = m_existingStatus->palette();
    if (!d.valid) {
        m_existingValid = false;
        m_openExisting->setEnabled(false);
        pal.setColor(QPalette::WindowText, Theme::t().danger);
        m_existingStatus->setPalette(pal);
        m_existingStatus->setText(T("未在该文件夹检测到有效的 Ivynote 笔记库（缺少有效的 vault.db）。"));
        m_existingStatus->show();
        return;
    }
    if (d.isRemote) {
        m_existingValid = false;
        m_openExisting->setEnabled(false);
        pal.setColor(QPalette::WindowText, Theme::t().warnInk);
        m_existingStatus->setPalette(pal);
        m_existingStatus->setText(T("该目录是同步笔记库的本地缓存，不能作为本地笔记库直接打开。"));
        m_existingStatus->show();
        return;
    }
    m_existingValid = true;
    m_openExisting->setEnabled(true);
    pal.setColor(QPalette::WindowText, Theme::t().ivy);
    m_existingStatus->setPalette(pal);
    const QString countText = (i18n::isZh() ? QStringLiteral("%1 条笔记记录") : QStringLiteral("%1 note records")).arg(d.noteRecords);
    const QString encText = d.encrypted ? T("已加密") : T("未加密");
    QString msg = QStringLiteral("✓ ") + d.name + QStringLiteral(" · ") + countText + QStringLiteral(" · ") + encText;
    if (!d.registeredId.isEmpty()) msg += QStringLiteral(" · ") + T("已在列表中，可直接打开");
    m_existingStatus->setText(msg);
    m_existingStatus->show();
}

void VaultDialog::startScan() {
    if (!m_scanWatcher || m_scanWatcher->isRunning()) return;
    m_existingScan->setEnabled(false);
    m_scanStatus->setText(T("正在扫描本机笔记库…"));
    m_scanWatcher->setFuture(QtConcurrent::run([] { return vaults::scanForVaults(); }));
}

void VaultDialog::applyScanResults(const QList<DiscoveredVault> &found) {
    m_discoveredList->clear();
    if (found.isEmpty()) {
        m_scanStatus->setText(T("未自动扫描到其他本地笔记库，你可以点击「选择文件夹…」手动指定。"));
        return;
    }
    m_scanStatus->setText((i18n::isZh()
        ? QStringLiteral("扫描到 %1 个本地笔记库（点击选择，双击直接打开）：")
        : QStringLiteral("Found %1 local vaults (click to select, double-click to open):")).arg(found.size()));
    for (const DiscoveredVault &d : found) {
        const QString countText = (i18n::isZh() ? QStringLiteral("%1 条笔记") : QStringLiteral("%1 notes")).arg(d.noteRecords);
        const QString encText = d.encrypted ? T("已加密") : T("未加密");
        QString head = d.name + QStringLiteral("  ·  ") + countText + QStringLiteral("  ·  ") + encText;
        if (!d.registeredId.isEmpty()) head += QStringLiteral("  [") + T("已添加") + u']';
        auto *item = new QListWidgetItem(icons::themed(QStringLiteral("database")),
                                         head + u'\n' + QDir::toNativeSeparators(d.dir),
                                         m_discoveredList);
        item->setData(Qt::UserRole, d.dir);
        item->setToolTip(QDir::toNativeSeparators(d.dir));
    }
}

void VaultDialog::openExisting() {
    const QString raw = m_existingPath->text().trimmed();
    if (raw.isEmpty()) return showError(m_existingError, T("请选择笔记库文件夹。"));
    showError(m_existingError, QString());
    VaultInfo attached;
    QString err;
    if (!vaults::attachExisting(raw, QString(), &attached, &err)) {
        return showError(m_existingError, err);
    }
    m_vaultId = attached.id;
    accept();
}

void VaultDialog::checkServer() {
    const QUrl url = ApiClient::normalizeServer(m_server->text());
    if (url.isEmpty()) return showError(m_serverError, T("请输入有效的服务器地址。"));
    showError(m_serverError, QString());
    delete m_api;
    m_api = new ApiClient(url, this);
    setBusy(true);
    QPointer<VaultDialog> self(this);
    m_api->get(QStringLiteral("/health"), this, [self, url](const ApiReply &r) {
        if (!self) return;
        if (!r.ok || !r.json.toObject().value(QLatin1String("ok")).toBool()) {
            self->setBusy(false);
            return self->showError(self->m_serverError, r.network() ? T("连接不上服务器，请检查地址和网络。") + QStringLiteral("\n") + r.error
                                                                    : T("这个地址不像 Ivynote 服务器。"));
        }
        self->m_api->get(QStringLiteral("/info"), self, [self, url](const ApiReply &r) {
            if (!self) return;
            self->setBusy(false);
            if (!r.ok) return self->showError(self->m_serverError, authErrorText(r.status, r.error));
            self->m_info = ServerInfo::fromJson(r.json.toObject());
            const QString site = self->m_info.siteName.isEmpty() ? T("常青笔记") : self->m_info.siteName;
            self->m_siteTitle->setText(site);
            QString note = url.host() + (self->m_info.version.isEmpty() ? QString() : QStringLiteral(" · ") + self->m_info.version);
            if (!self->m_info.hasUsers) note += u'\n' + T("这是一台新服务器：第一个注册的账户将成为管理员。");
            self->m_siteNote->setText(note);
            self->m_mode->setTabVisible(0, self->m_info.hasUsers);
            self->m_mode->setTabVisible(1, self->m_info.allowRegistration || !self->m_info.hasUsers);
            self->m_mode->setCurrentIndex(self->m_info.hasUsers ? 0 : 1);
            emit self->m_mode->currentChanged(self->m_mode->currentIndex());
            self->go(Account);
            self->m_identity->setFocus();
        });
    }, 15000);
}

void VaultDialog::submitAccount() {
    const bool reg = m_mode->currentIndex() == 1;
    const QString identity = m_identity->text().trimmed();
    const QString password = m_password->text();
    if (identity.isEmpty() || password.isEmpty()) return showError(m_accountError, T("请填写用户名和密码。"));
    if (reg && (password.size() < 8 || password.toUtf8().size() > 72)) return showError(m_accountError, T("密码至少 8 个字符，最多 72 字节。"));
    QJsonObject body;
    if (reg) {
        body = {{"username", identity}, {"password", password}};
        if (!m_email->text().trimmed().isEmpty()) body.insert(QStringLiteral("email"), m_email->text().trimmed());
        if (!m_displayName->text().trimmed().isEmpty()) body.insert(QStringLiteral("displayName"), m_displayName->text().trimmed());
    } else {
        body = {{"identity", identity}, {"password", password}};
    }
    setBusy(true);
    showError(m_accountError, QString());
    QPointer<VaultDialog> self(this);
    m_api->postJson(reg ? QStringLiteral("/auth/register") : QStringLiteral("/auth/login"), body, this, [self](const ApiReply &r) {
        if (!self) return;
        self->setBusy(false);
        if (!r.ok) return self->showError(self->m_accountError, authErrorText(r.status, r.error));
        const QString token = r.json.toObject().value(QLatin1String("token")).toString();
        const User user = User::fromJson(r.json.toObject().value(QLatin1String("user")).toObject());
        if (token.isEmpty() || user.id.isEmpty()) return self->showError(self->m_accountError, T("服务器的回应不完整。"));
        const QString server = self->m_api->server().toString();
        // One vault per account and server: reuse it rather than duplicate data.
        for (const VaultInfo &v : vaults::list()) {
            if (v.isRemote() && v.server == server && v.userId == user.id) {
                secrets::set(v.tokenKey(), token.toUtf8());
                self->m_vaultId = v.id;
                return self->accept();
            }
        }
        VaultInfo v;
        v.kind = VaultInfo::Remote;
        v.server = server;
        v.siteName = self->m_info.siteName;
        v.userId = user.id;
        v.username = user.username;
        v.user = user.toJson();
        v.name = (user.displayName.isEmpty() ? user.username : user.displayName) + QStringLiteral(" @ ") +
                 (self->m_info.siteName.isEmpty() ? self->m_api->server().host() : self->m_info.siteName);
        v.encrypted = self->m_remoteEncrypt->isChecked();
        QString err;
        if (!vaults::create(v, &err)) return self->showError(self->m_accountError, err);
        secrets::set(v.tokenKey(), token.toUtf8());
        self->m_vaultId = v.id;
        self->accept();
    }, 20000);
}

// --- sign in again ---

SignInDialog::SignInDialog(const VaultInfo &vault, QWidget *parent) : QDialog(parent), m_vault(vault), m_api(new ApiClient(QUrl(vault.server), this)) {
    setWindowTitle(T("重新登录"));
    setMinimumWidth(400);
    auto *col = new QVBoxLayout(this);
    col->setContentsMargins(28, 24, 28, 24);
    col->setSpacing(10);
    col->addWidget(new LeafWidget(48, 1.0, this), 0, Qt::AlignHCenter);
    col->addWidget(title(vault.name, this));
    auto *note = mutedLabel(T("登录已过期。未同步的更改都保存在本机，登录后会继续同步。"), this, 12.5);
    note->setWordWrap(true);
    note->setAlignment(Qt::AlignHCenter);
    col->addWidget(note);
    auto *server = mutedLabel(QUrl(vault.server).host(), this, 12);
    server->setAlignment(Qt::AlignHCenter);
    col->addWidget(server);
    m_identity = field(T("用户名或邮箱"), this);
    m_identity->setText(vault.username);
    m_password = field(T("密码"), this);
    m_password->setEchoMode(QLineEdit::Password);
    col->addWidget(m_identity);
    col->addWidget(m_password);
    m_error = errorLabel(this);
    col->addWidget(m_error);
    auto *row = new QHBoxLayout;
    auto *later = new PillButton(T("稍后"), PillButton::Ghost, this);
    m_submit = new PillButton(T("登录"), PillButton::Primary, this);
    connect(later, &QToolButton::clicked, this, &QDialog::reject);
    connect(m_submit, &QToolButton::clicked, this, &SignInDialog::submit);
    connect(m_password, &QLineEdit::returnPressed, this, &SignInDialog::submit);
    row->addWidget(later);
    row->addStretch(1);
    row->addWidget(m_submit);
    col->addLayout(row);
    m_password->setFocus();
}

void SignInDialog::submit() {
    if (m_identity->text().trimmed().isEmpty() || m_password->text().isEmpty()) return;
    m_submit->setEnabled(false);
    m_error->hide();
    QPointer<SignInDialog> self(this);
    m_api->postJson(QStringLiteral("/auth/login"), QJsonObject{{"identity", m_identity->text().trimmed()}, {"password", m_password->text()}}, this,
                    [self](const ApiReply &r) {
                        if (!self) return;
                        self->m_submit->setEnabled(true);
                        if (!r.ok) {
                            self->m_error->setText(authErrorText(r.status, r.error));
                            self->m_error->show();
                            return;
                        }
                        const User u = User::fromJson(r.json.toObject().value(QLatin1String("user")).toObject());
                        if (!self->m_vault.userId.isEmpty() && u.id != self->m_vault.userId) {
                            // Do not leave the other account's new session open.
                            self->m_api->setToken(r.json.toObject().value(QLatin1String("token")).toString());
                            self->m_api->postJson(QStringLiteral("/auth/logout"), QJsonObject(), self, [](const ApiReply &) {});
                            self->m_api->setToken(QString());
                            self->m_error->setText(T("这是另一个账户。这个笔记库只能用原来的账户登录。"));
                            self->m_error->show();
                            return;
                        }
                        self->m_token = r.json.toObject().value(QLatin1String("token")).toString();
                        self->m_user = u;
                        self->accept();
                    }, 20000);
}

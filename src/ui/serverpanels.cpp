#include "ui/serverpanels.h"

#include "core/i18n.h"
#include "sync/api.h"
#include "sync/engine.h"
#include "vault/session.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

void serverpanels::calendar(QWidget *parent, Session *session) {
    QDialog dialog(parent);
    dialog.setWindowTitle(T("日历订阅"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *url = new QLineEdit;
    url->setReadOnly(true);
    auto *status = new QLabel;
    status->setTextFormat(Qt::PlainText);
    auto *copy = new QPushButton(T("复制订阅链接"));
    auto *change = new QPushButton;
    auto *stop = new QPushButton(T("停用订阅"));
    auto *refresh = new QPushButton(T("刷新"));
    layout->addWidget(status);
    layout->addWidget(url);
    layout->addWidget(copy);
    layout->addWidget(change);
    layout->addWidget(stop);
    layout->addWidget(refresh);
    const auto request = [&](const QByteArray &method) {
        layout->parentWidget()->setEnabled(false);
        session->api()->send(method, QStringLiteral("/me/calendar"), method == "POST" ? QByteArray("{}") : QByteArray(),
            &dialog, [&, method](const ApiReply &r) {
                dialog.setEnabled(true);
                const auto data = r.json.toObject();
                const bool active = method != "DELETE" && data.value("enabled").toBool();
                url->setText(r.ok && active ? data.value("url").toString() : QString());
                copy->setEnabled(!url->text().isEmpty());
                stop->setEnabled(r.ok && active);
                change->setEnabled(r.ok);
                change->setText(active ? T("重置订阅链接") : T("启用订阅"));
                status->setText(r.ok ? (active ? T("持有链接的人可以读取日程") : T("订阅已停用")) : r.error);
            });
    };
    QObject::connect(copy, &QPushButton::clicked, &dialog, [url] { QApplication::clipboard()->setText(url->text()); });
    QObject::connect(change, &QPushButton::clicked, &dialog, [&] {
        if (!stop->isEnabled() || QMessageBox::question(&dialog, dialog.windowTitle(), T("重置后，旧订阅链接将失效。继续吗？")) == QMessageBox::Yes)
            request("POST");
    });
    QObject::connect(stop, &QPushButton::clicked, &dialog, [&] {
        if (QMessageBox::question(&dialog, dialog.windowTitle(), T("停用当前日历订阅？")) == QMessageBox::Yes) request("DELETE");
    });
    QObject::connect(refresh, &QPushButton::clicked, &dialog, [&] { request("GET"); });
    request("GET");
    dialog.exec();
}

void serverpanels::publish(QWidget *parent, Session *session) {
    QDialog dialog(parent);
    dialog.setWindowTitle(T("发布到博客"));
    auto *layout = new QFormLayout(&dialog);
    auto *sites = new QComboBox;
    auto *targets = new QComboBox;
    auto *folders = new QComboBox;
    for (const auto &folder : session->engine()->folders())
        if (!folder.deleted) folders->addItem(folder.name, folder.id);
    auto *addSite = new QPushButton(T("添加 WordPress 站点"));
    auto *addTarget = new QPushButton(T("创建发布目标"));
    auto *push = new QPushButton(T("预览并发布"));
    auto *status = new QLabel;
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    layout->addRow(T("站点"), sites);
    layout->addRow(addSite);
    layout->addRow(T("笔记本"), folders);
    layout->addRow(addTarget);
    layout->addRow(T("发布目标"), targets);
    layout->addRow(push);
    layout->addRow(status);
    const auto load = [&](const QString &path, QComboBox *box) {
        session->api()->get(path, &dialog, [box, status](const ApiReply &r) {
            if (!r.ok) { status->setText(r.error); return; }
            box->clear();
            for (const auto &item : r.json.toArray()) {
                auto obj = item.toObject();
                box->addItem(obj.value("name").toString(), obj.value("id").toString());
            }
        });
    };
    const QString sitePath = QStringLiteral("/publish/sites");
    const QString targetPath = QStringLiteral("/publish/targets");
    load(sitePath, sites);
    load(targetPath, targets);
    QObject::connect(addSite, &QPushButton::clicked, &dialog, [&] {
        QDialog form(&dialog);
        form.setWindowTitle(addSite->text());
        QFormLayout fields(&form);
        QLineEdit name, url, username, password;
        password.setEchoMode(QLineEdit::Password);
        fields.addRow(T("名称"), &name);
        fields.addRow(QStringLiteral("WordPress URL"), &url);
        fields.addRow(T("用户名"), &username);
        fields.addRow(T("应用密码"), &password);
        QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        fields.addRow(&buttons);
        QObject::connect(&buttons, &QDialogButtonBox::accepted, &form, &QDialog::accept);
        QObject::connect(&buttons, &QDialogButtonBox::rejected, &form, &QDialog::reject);
        if (form.exec() != QDialog::Accepted) return;
        addSite->setEnabled(false);
        session->api()->postJson(sitePath, QJsonObject{{"name", name.text()}, {"url", url.text().trimmed()},
            {"username", username.text()}, {"password", password.text()}}, &dialog, [&](const ApiReply &r) {
                addSite->setEnabled(true);
                status->setText(r.error);
                if (r.ok) load(sitePath, sites);
            });
    });
    QObject::connect(addTarget, &QPushButton::clicked, &dialog, [&] {
        if (sites->currentIndex() < 0 || folders->currentIndex() < 0) { status->setText(T("请选择站点和笔记本")); return; }
        addTarget->setEnabled(false);
        session->api()->postJson(targetPath, QJsonObject{{"name", folders->currentText()}, {"siteId", sites->currentData().toString()},
            {"folderId", folders->currentData().toString()}, {"format", "html"}, {"enabled", false},
            {"categories", QJsonArray()}, {"tags", QJsonArray()}}, &dialog, [&](const ApiReply &r) {
                addTarget->setEnabled(true);
                status->setText(r.error);
                if (r.ok) load(targetPath, targets);
            });
    });
    QObject::connect(push, &QPushButton::clicked, &dialog, [&] {
        if (targets->currentIndex() < 0) return;
        const QString path = targetPath + u'/' + targets->currentData().toString();
        const QString name = targets->currentText();
        push->setEnabled(false);
        session->api()->get(path + QStringLiteral("/preview"), &dialog, [&, path, name](const ApiReply &r) {
            push->setEnabled(true);
            if (!r.ok) { status->setText(r.error); return; }
            const auto obj = r.json.toObject();
            if (QMessageBox::question(&dialog, name, T("将新增 %1 篇、更新 %2 篇文章。确认发布？")
                .arg(obj.value("new").toInt()).arg(obj.value("changed").toInt())) != QMessageBox::Yes) return;
            push->setEnabled(false);
            session->api()->postJson(path + QStringLiteral("/run"), QJsonObject{{"confirm", true}}, &dialog, [&](const ApiReply &result) {
                push->setEnabled(true);
                const auto run = result.json.toObject();
                status->setText(result.ok ? T("发布结果：%1，新增 %2，更新 %3，失败 %4").arg(run.value("status").toString())
                    .arg(run.value("created").toInt()).arg(run.value("updated").toInt()).arg(run.value("failed").toInt()) : result.error);
            });
        });
    });
    dialog.exec();
}

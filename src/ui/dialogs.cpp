#include "ui/dialogs.h"

#include "core/i18n.h"
#include "core/models.h"
#include "core/notelock.h"
#include "core/tree.h"
#include "storage/crypto.h"
#include "sync/engine.h"
#include "ui/icons.h"
#include "ui/theme.h"
#include "ui/toast.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace dialogs {

bool pickFolder(SyncEngine *engine, QWidget *parent, const QString &title, const QString &current, QString *out,
                const QString &excludeTree) {
    QDialog dlg(parent);
    dlg.setWindowTitle(title);
    dlg.resize(380, 440);
    auto *col = new QVBoxLayout(&dlg);
    auto *filter = new QLineEdit(&dlg);
    filter->setPlaceholderText(T("筛选笔记本"));
    filter->setClearButtonEnabled(true);
    col->addWidget(filter);
    auto *tree = new QTreeWidget(&dlg);
    tree->setHeaderHidden(true);
    tree->setIconSize(QSize(16, 16));
    col->addWidget(tree, 1);
    auto *root = new QTreeWidgetItem(tree, {T("笔记（根目录）")});
    root->setData(0, Qt::UserRole, QString());
    root->setIcon(0, icons::icon(QStringLiteral("note"), Theme::t().muted));
    QHash<QString, QList<const Folder *>> kids;
    QSet<QString> ids;
    for (const Folder *f : engine->folderList()) ids.insert(f->id);
    for (const Folder *f : sortFolders(engine->folderList())) kids[ids.contains(f->parentId) ? f->parentId : QString()] << f;
    QTreeWidgetItem *selected = root;
    std::function<void(QTreeWidgetItem *, const QString &, int)> add = [&](QTreeWidgetItem *parentItem, const QString &pid, int depth) {
        for (const Folder *f : kids.value(pid)) {
            if (!excludeTree.isEmpty() && f->id == excludeTree) continue;
            auto *it = new QTreeWidgetItem(parentItem, {f->name});
            it->setData(0, Qt::UserRole, f->id);
            it->setIcon(0, icons::icon(QStringLiteral("folder-fill"), Theme::t().ivy));
            if (f->id == current) selected = it;
            if (depth < 64) add(it, f->id, depth + 1);
        }
    };
    add(root, QString(), 0);
    tree->expandAll();
    tree->setCurrentItem(selected);
    QObject::connect(filter, &QLineEdit::textChanged, tree, [tree](const QString &q) {
        std::function<bool(QTreeWidgetItem *)> walk = [&](QTreeWidgetItem *it) {
            bool any = q.isEmpty() || it->text(0).contains(q, Qt::CaseInsensitive);
            for (int i = 0; i < it->childCount(); ++i) any = walk(it->child(i)) || any;
            it->setHidden(!any);
            return any;
        };
        for (int i = 0; i < tree->topLevelItemCount(); ++i) walk(tree->topLevelItem(i));
    });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(T("移动"));
    buttons->button(QDialogButtonBox::Cancel)->setText(T("取消"));
    col->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    QObject::connect(tree, &QTreeWidget::itemDoubleClicked, &dlg, &QDialog::accept);
    if (dlg.exec() != QDialog::Accepted || !tree->currentItem()) return false;
    *out = tree->currentItem()->data(0, Qt::UserRole).toString();
    return true;
}

namespace {

class TagPopup : public QWidget {
public:
    TagPopup(SyncEngine *engine, const QString &noteId, QWidget *parent)
        : QWidget(parent, Qt::Popup), m_engine(engine), m_noteId(noteId) {
        setAttribute(Qt::WA_DeleteOnClose);
        setFixedWidth(260);
        auto *col = new QVBoxLayout(this);
        col->setContentsMargins(8, 8, 8, 8);
        col->setSpacing(6);
        m_filter = new QLineEdit(this);
        m_filter->setPlaceholderText(T("搜索或新建标签"));
        m_filter->setClearButtonEnabled(true);
        col->addWidget(m_filter);
        m_list = new QListWidget(this);
        m_list->setFrameShape(QFrame::NoFrame);
        m_list->setMinimumHeight(180);
        col->addWidget(m_list);
        connect(m_filter, &QLineEdit::textChanged, this, [this] { fill(); });
        connect(m_filter, &QLineEdit::returnPressed, this, [this] {
            if (m_list->count() > 0) activate(m_list->currentItem() ? m_list->currentItem() : m_list->item(0));
        });
        connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) { activate(it); });
        m_filter->installEventFilter(this);
        fill();
    }

protected:
    bool eventFilter(QObject *o, QEvent *e) override {
        if (o == m_filter && e->type() == QEvent::KeyPress) {
            auto *k = static_cast<QKeyEvent *>(e);
            if (k->key() == Qt::Key_Down || k->key() == Qt::Key_Up) {
                const int row = std::clamp(m_list->currentRow() + (k->key() == Qt::Key_Down ? 1 : -1), 0, m_list->count() - 1);
                m_list->setCurrentRow(row);
                return true;
            }
        }
        return QWidget::eventFilter(o, e);
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), Theme::t().surface);
        p.setPen(Theme::t().line);
        p.drawRect(rect().adjusted(0, 0, -1, -1));
    }

private:
    void fill() {
        m_list->clear();
        const Note *n = m_engine->note(m_noteId);
        if (!n) {
            close();
            return;
        }
        const QString q = m_filter->text().trimmed().remove(QRegularExpression(QStringLiteral("^#+")));
        QList<const Tag *> tags = m_engine->tagList();
        std::sort(tags.begin(), tags.end(), [](const Tag *a, const Tag *b) { return a->name.localeAwareCompare(b->name) < 0; });
        bool exact = false;
        for (const Tag *t : tags) {
            if (!q.isEmpty() && !t->name.contains(q, Qt::CaseInsensitive)) continue;
            exact = exact || t->name.compare(q, Qt::CaseInsensitive) == 0;
            auto *it = new QListWidgetItem(QStringLiteral("#") + t->name, m_list);
            it->setData(Qt::UserRole, t->id);
            it->setCheckState(n->tags.contains(t->id) ? Qt::Checked : Qt::Unchecked);
            it->setForeground(Theme::t().ink2);
        }
        if (!q.isEmpty() && !exact) {
            auto *it = new QListWidgetItem(icons::themed(QStringLiteral("plus")), (i18n::isZh() ? QStringLiteral("新建标签 #%1") : QStringLiteral("Create tag #%1")).arg(q), m_list);
            it->setData(Qt::UserRole + 1, q.left(50));
        }
        if (m_list->count()) m_list->setCurrentRow(0);
    }

    void activate(QListWidgetItem *it) {
        const Note *n = m_engine->note(m_noteId);
        if (!n || !it) return;
        QStringList tags = n->tags;
        const QString create = it->data(Qt::UserRole + 1).toString();
        if (!create.isEmpty()) {
            if (tags.size() >= 50) {
                Toaster::show(T("一篇笔记最多 50 个标签"), true);
                return;
            }
            tags << m_engine->createTag(create).id;
            m_filter->clear();
        } else {
            const QString id = it->data(Qt::UserRole).toString();
            if (tags.contains(id)) tags.removeAll(id);
            else if (tags.size() < 50) tags << id;
        }
        NotePatch p;
        p.tags = tags;
        m_engine->updateNote(m_noteId, p);
        fill();
    }

    SyncEngine *m_engine;
    QString m_noteId;
    QLineEdit *m_filter;
    QListWidget *m_list;
};

} // namespace

void tagPicker(SyncEngine *engine, const QString &noteId, const QPoint &global, QWidget *parent) {
    auto *popup = new TagPopup(engine, noteId, parent);
    popup->adjustSize();
    QPoint at = global;
    if (QScreen *s = QApplication::screenAt(global)) {
        const QRect avail = s->availableGeometry();
        at.setX(std::min(at.x(), avail.right() - popup->width()));
        at.setY(std::min(at.y(), avail.bottom() - popup->height()));
    }
    popup->move(at);
    popup->show();
}

void showRecoveryKey(QWidget *parent, const QString &vaultName, const QString &key, bool justCreated) {
    QDialog dlg(parent);
    dlg.setWindowTitle(T("恢复密钥"));
    auto *col = new QVBoxLayout(&dlg);
    col->setSpacing(12);
    auto *intro = new QLabel(&dlg);
    intro->setWordWrap(true);
    intro->setText((justCreated ? T("「%1」的本机数据已加密，密钥保存在系统钥匙串中。") : T("「%1」的恢复密钥：")).arg(vaultName) + u' ' +
                   T("如果钥匙串丢失（重装系统、换电脑后复制数据），只能用这把恢复密钥打开笔记库。请把它抄下来或存进密码管理器。"));
    col->addWidget(intro);
    auto *field = new QLineEdit(key, &dlg);
    field->setReadOnly(true);
    field->setFont(Theme::mono(14));
    field->setMinimumWidth(QFontMetrics(field->font()).horizontalAdvance(key) + 40);
    col->addWidget(field);
    auto *buttons = new QDialogButtonBox(&dlg);
    auto *copy = buttons->addButton(T("复制"), QDialogButtonBox::ActionRole);
    auto *done = buttons->addButton(T("完成"), QDialogButtonBox::AcceptRole);
    QObject::connect(copy, &QPushButton::clicked, &dlg, [key] {
        QApplication::clipboard()->setText(key);
        Toaster::show(T("已复制"));
    });
    QObject::connect(done, &QPushButton::clicked, &dlg, &QDialog::accept);
    col->addWidget(buttons);
    dlg.exec();
}

QByteArray askRecoveryKey(QWidget *parent, const QString &vaultName, bool wrongBefore) {
    QDialog dlg(parent);
    dlg.setWindowTitle(T("解锁笔记库"));
    auto *col = new QVBoxLayout(&dlg);
    auto *intro = new QLabel(&dlg);
    intro->setWordWrap(true);
    intro->setText(T("系统钥匙串里找不到「%1」的密钥。请输入创建笔记库时保存的恢复密钥。").arg(vaultName));
    col->addWidget(intro);
    auto *field = new QLineEdit(&dlg);
    field->setFont(Theme::mono(14));
    field->setPlaceholderText(QStringLiteral("XXXX-XXXX-…"));
    col->addWidget(field);
    auto *error = new QLabel(wrongBefore ? T("这把密钥打不开这个笔记库。") : QString(), &dlg);
    QPalette pal = error->palette();
    pal.setColor(QPalette::WindowText, Theme::t().danger);
    error->setPalette(pal);
    error->setVisible(wrongBefore);
    col->addWidget(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(T("解锁"));
    buttons->button(QDialogButtonBox::Cancel)->setText(T("取消"));
    col->addWidget(buttons);
    QByteArray key;
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, [&] {
        key = VaultCipher::decodeRecoveryKey(field->text());
        if (key.isEmpty()) {
            error->setText(T("密钥格式不正确。"));
            error->show();
            return;
        }
        dlg.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return {};
    return key;
}

bool confirm(QWidget *parent, const QString &title, const QString &text, const QString &yes) {
    QMessageBox box(QMessageBox::Warning, title, text, QMessageBox::NoButton, parent);
    QPushButton *ok = box.addButton(yes, QMessageBox::DestructiveRole);
    box.addButton(T("取消"), QMessageBox::RejectRole);
    box.setDefaultButton(qobject_cast<QPushButton *>(box.buttons().last()));
    box.exec();
    return box.clickedButton() == ok;
}

bool askNotePassword(QWidget *parent, bool changing, QString *password, QString *hint) {
    QDialog dlg(parent);
    dlg.setWindowTitle(changing ? T("修改笔记密码") : T("给笔记设置密码"));
    dlg.setMinimumWidth(420);
    auto *col = new QVBoxLayout(&dlg);
    auto *intro = new QLabel(&dlg);
    intro->setWordWrap(true);
    intro->setText(T("加密后，正文只在你的设备上解密，服务器看不到内容。请注意：\n"
                     "• 标题和附件不会加密\n"
                     "• 这篇笔记已有的历史版本会被删除，公开分享会关闭\n"
                     "• 忘记密码无法恢复，笔记内容将永远无法打开"));
    col->addWidget(intro);
    auto *pw = new QLineEdit(&dlg);
    pw->setEchoMode(QLineEdit::Password);
    pw->setPlaceholderText(T("密码（至少 %1 个字符）").arg(notelock::kMinPassword));
    auto *again = new QLineEdit(&dlg);
    again->setEchoMode(QLineEdit::Password);
    again->setPlaceholderText(T("再输一遍"));
    auto *hintField = new QLineEdit(&dlg);
    hintField->setMaxLength(100);
    hintField->setPlaceholderText(T("密码提示（可选，不要写密码本身）"));
    col->addWidget(pw);
    col->addWidget(again);
    col->addWidget(hintField);
    auto *ack = new QCheckBox(T("我已了解，忘记密码无法恢复"), &dlg);
    col->addWidget(ack);
    auto *error = new QLabel(&dlg);
    QPalette pal = error->palette();
    pal.setColor(QPalette::WindowText, Theme::t().danger);
    error->setPalette(pal);
    error->hide();
    col->addWidget(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QPushButton *ok = buttons->button(QDialogButtonBox::Ok);
    ok->setText(changing ? T("修改密码") : T("加密笔记"));
    ok->setEnabled(false);
    buttons->button(QDialogButtonBox::Cancel)->setText(T("取消"));
    col->addWidget(buttons);
    auto check = [&] {
        QString problem;
        if (!pw->text().isEmpty() && pw->text().size() < notelock::kMinPassword) problem = T("密码太短。");
        else if (!again->text().isEmpty() && pw->text() != again->text()) problem = T("两次输入的密码不一致。");
        error->setText(problem);
        error->setVisible(!problem.isEmpty());
        ok->setEnabled(problem.isEmpty() && pw->text().size() >= notelock::kMinPassword && pw->text() == again->text() && ack->isChecked());
    };
    QObject::connect(pw, &QLineEdit::textChanged, &dlg, check);
    QObject::connect(again, &QLineEdit::textChanged, &dlg, check);
    QObject::connect(ack, &QCheckBox::toggled, &dlg, check);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, [&] { if (ok->isEnabled()) dlg.accept(); });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return false;
    *password = pw->text();
    *hint = hintField->text().trimmed();
    return true;
}

} // namespace dialogs

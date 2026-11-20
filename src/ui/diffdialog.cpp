#include "ui/diffdialog.h"

#include "core/diff.h"
#include "core/format.h"
#include "core/i18n.h"
#include "core/models.h"
#include "core/notelock.h"
#include "sync/engine.h"
#include "ui/dialogs.h"
#include "ui/icons.h"
#include "ui/theme.h"
#include "ui/toast.h"
#include "ui/widgets.h"
#include "vault/session.h"

#include <QCheckBox>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

namespace {

QString escapePre(QString s) {
    s = s.toHtmlEscaped();
    s.replace(u' ', QStringLiteral("&nbsp;"));
    s.replace(u'\t', QStringLiteral("&nbsp;&nbsp;&nbsp;&nbsp;"));
    return s.isEmpty() ? QStringLiteral("&nbsp;") : s;
}

QString renderSpans(const diff::Line &line, const QString &strongDelBg, const QString &strongAddBg) {
    if (line.spans.isEmpty()) return escapePre(line.text);
    QString out;
    for (const diff::Span &sp : line.spans) {
        const QString txt = escapePre(sp.text);
        if (sp.kind == diff::Kind::Del)
            out += QStringLiteral("<span style=\"background-color:%1;border-radius:2px;\">%2</span>").arg(strongDelBg, txt);
        else if (sp.kind == diff::Kind::Add)
            out += QStringLiteral("<span style=\"background-color:%1;border-radius:2px;\">%2</span>").arg(strongAddBg, txt);
        else
            out += txt;
    }
    return out.isEmpty() ? QStringLiteral("&nbsp;") : out;
}

QString rgba(const QColor &c, qreal a) {
    return QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(a, 0, 'f', 2);
}

QString noteLabel(const Note *n) {
    if (!n) return T("无标题");
    return n->title.trimmed().isEmpty() ? T("无标题") : n->title;
}

} // namespace

NoteDiffDialog::NoteDiffDialog(Session *session, const QString &baseNoteId, const QString &initialTargetId, QWidget *parent)
    : QDialog(parent), m_session(session), m_baseId(baseNoteId) {
    setWindowTitle(T("对比笔记差异"));
    resize(920, 640);
    setMinimumSize(700, 480);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 14, 16, 14);
    root->setSpacing(10);

    // Header cards: left (current) vs right (compared).
    auto *headerRow = new QHBoxLayout;
    headerRow->setSpacing(10);

    auto makeCard = [this](const QString &caption, QLabel **titleOut, QLabel **badgeOut, QPushButton **btnOut) {
        auto *card = new QFrame(this);
        card->setFrameShape(QFrame::StyledPanel);
        auto *row = new QHBoxLayout(card);
        row->setContentsMargins(12, 8, 10, 8);
        row->setSpacing(8);
        auto *col = new QVBoxLayout;
        col->setSpacing(2);
        auto *capRow = new QHBoxLayout;
        capRow->setSpacing(6);
        capRow->addWidget(mutedLabel(caption, card, 11.5));
        if (badgeOut) {
            auto *badge = new QLabel(T("冲突副本"), card);
            badge->setFont(Theme::uiFont(11, QFont::Medium));
            badge->setStyleSheet(QStringLiteral("QLabel{background:%1;color:%2;border-radius:3px;padding:1px 6px;}")
                                     .arg(Theme::t().warnSoft.name(), Theme::t().warnInk.name()));
            badge->hide();
            capRow->addWidget(badge);
            *badgeOut = badge;
        }
        capRow->addStretch(1);
        col->addLayout(capRow);
        auto *title = new QLabel(card);
        title->setFont(Theme::uiFont(13.5, QFont::DemiBold));
        title->setTextInteractionFlags(Qt::TextSelectableByMouse);
        col->addWidget(title);
        *titleOut = title;
        row->addLayout(col, 1);
        if (btnOut) {
            auto *btn = new QPushButton(T("切换笔记"), card);
            btn->setObjectName(QStringLiteral("diffSwitchNote"));
            row->addWidget(btn);
            *btnOut = btn;
        }
        return card;
    };

    headerRow->addWidget(makeCard(T("当前笔记（基准）"), &m_baseTitle, nullptr, nullptr), 1);
    headerRow->addWidget(makeCard(T("对比笔记"), &m_targetTitle, &m_conflictBadge, &m_switchBtn), 1);
    root->addLayout(headerRow);

    // Controls bar: stats, changes-only filter, split/unified switch.
    m_controlsBar = new QWidget(this);
    auto *ctrl = new QHBoxLayout(m_controlsBar);
    ctrl->setContentsMargins(2, 0, 2, 0);
    ctrl->setSpacing(12);
    m_stats = new QLabel(m_controlsBar);
    m_stats->setObjectName(QStringLiteral("diffStats"));
    m_stats->setFont(Theme::uiFont(12.5));
    ctrl->addWidget(m_stats);
    ctrl->addStretch(1);

    m_onlyChangesBox = new QCheckBox(T("仅看差异行"), m_controlsBar);
    m_onlyChangesBox->setObjectName(QStringLiteral("diffChangesOnly"));
    connect(m_onlyChangesBox, &QCheckBox::toggled, this, [this](bool on) {
        m_changesOnly = on;
        refreshDiff();
    });
    ctrl->addWidget(m_onlyChangesBox);

    m_splitBtn = new QToolButton(m_controlsBar);
    m_splitBtn->setObjectName(QStringLiteral("diffSplitBtn"));
    m_splitBtn->setText(T("并排对比"));
    m_splitBtn->setCheckable(true);
    m_splitBtn->setChecked(true);
    m_unifiedBtn = new QToolButton(m_controlsBar);
    m_unifiedBtn->setObjectName(QStringLiteral("diffUnifiedBtn"));
    m_unifiedBtn->setText(T("合并视图"));
    m_unifiedBtn->setCheckable(true);
    connect(m_splitBtn, &QToolButton::clicked, this, [this] {
        m_unified = false;
        m_splitBtn->setChecked(true);
        m_unifiedBtn->setChecked(false);
        refreshDiff();
    });
    connect(m_unifiedBtn, &QToolButton::clicked, this, [this] {
        m_unified = true;
        m_splitBtn->setChecked(false);
        m_unifiedBtn->setChecked(true);
        refreshDiff();
    });
    ctrl->addWidget(m_splitBtn);
    ctrl->addWidget(m_unifiedBtn);
    root->addWidget(m_controlsBar);

    // Main stack: picker | diff view | identical state.
    m_stack = new QStackedWidget(this);

    // Page 0: Note picker
    m_pickerPage = new QWidget(m_stack);
    auto *pickCol = new QVBoxLayout(m_pickerPage);
    pickCol->setContentsMargins(0, 4, 0, 4);
    pickCol->setSpacing(8);
    m_pickerSearch = new QLineEdit(m_pickerPage);
    m_pickerSearch->setObjectName(QStringLiteral("diffPickerSearch"));
    m_pickerSearch->setPlaceholderText(T("搜索要对比的笔记标题或内容…"));
    m_pickerSearch->setClearButtonEnabled(true);
    pickCol->addWidget(m_pickerSearch);
    m_pickerList = new QListWidget(m_pickerPage);
    m_pickerList->setObjectName(QStringLiteral("diffPickerList"));
    pickCol->addWidget(m_pickerList, 1);
    connect(m_pickerSearch, &QLineEdit::textChanged, this, &NoteDiffDialog::refreshPicker);
    connect(m_pickerList, &QListWidget::itemActivated, this, [this](QListWidgetItem *it) {
        if (!it) return;
        const QString id = it->data(Qt::UserRole).toString();
        if (!id.isEmpty()) pickTarget(id);
    });
    connect(m_pickerList, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) {
        if (!it) return;
        const QString id = it->data(Qt::UserRole).toString();
        if (!id.isEmpty()) pickTarget(id);
    });
    m_stack->addWidget(m_pickerPage);

    // Page 1: Diff browser
    m_diffView = new QTextBrowser(m_stack);
    m_diffView->setObjectName(QStringLiteral("diffBrowser"));
    m_diffView->setOpenLinks(false);
    m_diffView->setFont(Theme::mono(12.5));
    m_stack->addWidget(m_diffView);

    // Page 2: Identical view
    m_identicalPage = new QWidget(m_stack);
    auto *idCol = new QVBoxLayout(m_identicalPage);
    idCol->addStretch(1);
    auto *checkIcon = new QLabel(m_identicalPage);
    checkIcon->setAlignment(Qt::AlignCenter);
    checkIcon->setPixmap(icons::pixmap(QStringLiteral("check"), Theme::t().ok, 36, devicePixelRatioF()));
    idCol->addWidget(checkIcon);
    auto *idTitle = new QLabel(T("两篇笔记正文完全一致"), m_identicalPage);
    idTitle->setAlignment(Qt::AlignCenter);
    idTitle->setFont(Theme::uiFont(15, QFont::DemiBold));
    idCol->addWidget(idTitle);
    auto *idHint = mutedLabel(T("没有发现任何正文差异，可以放心清理多余的冲突副本。"), m_identicalPage, 13);
    idHint->setAlignment(Qt::AlignCenter);
    idCol->addWidget(idHint);
    idCol->addStretch(1);
    m_stack->addWidget(m_identicalPage);

    root->addWidget(m_stack, 1);

    // Bottom resolution actions.
    m_actionsBar = new QWidget(this);
    auto *actRow = new QHBoxLayout(m_actionsBar);
    actRow->setContentsMargins(0, 4, 0, 0);
    actRow->setSpacing(8);
    m_openTargetBtn = new QPushButton(T("打开对比笔记"), m_actionsBar);
    m_openTargetBtn->setObjectName(QStringLiteral("diffOpenTarget"));
    m_appendBtn = new QPushButton(T("将右侧追加到当前"), m_actionsBar);
    m_appendBtn->setObjectName(QStringLiteral("diffAppendRight"));
    m_replaceBtn = new QPushButton(T("用右侧覆盖当前"), m_actionsBar);
    m_replaceBtn->setObjectName(QStringLiteral("diffReplaceRight"));
    m_trashBtn = new QPushButton(T("移到回收站（右侧副本）"), m_actionsBar);
    m_trashBtn->setObjectName(QStringLiteral("diffTrashRight"));
    auto *closeBtn = new QPushButton(T("关闭"), m_actionsBar);

    actRow->addWidget(m_openTargetBtn);
    actRow->addWidget(m_appendBtn);
    actRow->addWidget(m_replaceBtn);
    actRow->addWidget(m_trashBtn);
    actRow->addStretch(1);
    actRow->addWidget(closeBtn);
    root->addWidget(m_actionsBar);

    connect(m_switchBtn, &QPushButton::clicked, this, &NoteDiffDialog::showPicker);
    connect(m_openTargetBtn, &QPushButton::clicked, this, [this] {
        if (!m_targetId.isEmpty()) {
            const QString id = m_targetId;
            accept();
            emit openNoteRequested(id);
        }
    });
    connect(m_appendBtn, &QPushButton::clicked, this, &NoteDiffDialog::appendRight);
    connect(m_replaceBtn, &QPushButton::clicked, this, &NoteDiffDialog::replaceWithRight);
    connect(m_trashBtn, &QPushButton::clicked, this, &NoteDiffDialog::trashRight);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);

    // Initial selection: explicit initialTargetId -> first counterpart -> picker.
    const Note *base = m_session ? m_session->engine()->note(m_baseId) : nullptr;
    m_baseTitle->setText(noteLabel(base));
    QString picked;
    if (m_session && !initialTargetId.isEmpty() && initialTargetId != m_baseId) {
        if (const Note *t = m_session->engine()->note(initialTargetId); t && !t->deleted && !t->trashed())
            picked = t->id;
    }
    if (picked.isEmpty() && m_session && base) {
        const auto counterparts = diff::findConflictCounterparts(*base, m_session->engine()->notes());
        if (!counterparts.isEmpty()) picked = counterparts.first()->id;
    }
    if (!picked.isEmpty()) pickTarget(picked);
    else showPicker();
}

void NoteDiffDialog::showPicker() {
    m_controlsBar->hide();
    m_openTargetBtn->setEnabled(false);
    m_appendBtn->setEnabled(false);
    m_replaceBtn->setEnabled(false);
    m_trashBtn->hide();
    m_targetTitle->setText(T("请选择要对比的笔记"));
    m_conflictBadge->hide();
    m_switchBtn->setText(T("选择笔记"));
    m_stack->setCurrentWidget(m_pickerPage);
    refreshPicker();
    m_pickerSearch->setFocus();
}

void NoteDiffDialog::refreshPicker() {
    m_pickerList->clear();
    if (!m_session) return;
    const auto &notes = m_session->engine()->notes();
    const Note *base = m_session->engine()->note(m_baseId);
    const QString q = m_pickerSearch->text().trimmed().toLower();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    QSet<QString> counterpartIds;
    if (base) {
        const auto counterparts = diff::findConflictCounterparts(*base, notes);
        QList<const Note *> shown;
        for (const Note *n : counterparts) {
            if (!q.isEmpty() && !n->title.toLower().contains(q) && !n->body.toLower().contains(q)) continue;
            shown.append(n);
            counterpartIds.insert(n->id);
        }
        if (!shown.isEmpty()) {
            auto *hdr = new QListWidgetItem(T("建议对比（冲突关联笔记）"), m_pickerList);
            hdr->setFlags(Qt::NoItemFlags);
            hdr->setForeground(Theme::t().warnInk);
            hdr->setFont(Theme::uiFont(12, QFont::Medium));
            for (const Note *n : shown) {
                auto *it = new QListWidgetItem(noteLabel(n) + QStringLiteral("   ·   ") + fmt::shortTime(n->updatedAt, now), m_pickerList);
                it->setData(Qt::UserRole, n->id);
                it->setIcon(icons::themed(QStringLiteral("file")));
            }
        }
    }

    QList<const Note *> rest;
    for (const Note &n : notes) {
        if (n.id == m_baseId || n.deleted || n.trashed() || counterpartIds.contains(n.id)) continue;
        if (!q.isEmpty() && !n.title.toLower().contains(q) && !n.body.toLower().contains(q)) continue;
        rest.append(&n);
    }
    std::sort(rest.begin(), rest.end(), [](const Note *a, const Note *b) { return a->updatedAt > b->updatedAt; });
    if (rest.size() > 60) rest.resize(60);

    if (!rest.isEmpty()) {
        auto *hdr = new QListWidgetItem(T("全部笔记"), m_pickerList);
        hdr->setFlags(Qt::NoItemFlags);
        hdr->setForeground(Theme::t().muted);
        hdr->setFont(Theme::uiFont(12, QFont::Medium));
        for (const Note *n : rest) {
            auto *it = new QListWidgetItem(noteLabel(n) + QStringLiteral("   ·   ") + fmt::shortTime(n->updatedAt, now), m_pickerList);
            it->setData(Qt::UserRole, n->id);
            it->setIcon(icons::themed(QStringLiteral("file")));
        }
    }
}

void NoteDiffDialog::pickTarget(const QString &id) {
    m_targetId = id;
    m_switchBtn->setText(T("切换笔记"));
    refreshDiff();
}

void NoteDiffDialog::refreshDiff() {
    if (!m_session) return;
    const Note *base = m_session->engine()->note(m_baseId);
    const Note *target = m_session->engine()->note(m_targetId);
    if (!base || !target) {
        showPicker();
        return;
    }

    m_baseTitle->setText(noteLabel(base));
    m_targetTitle->setText(noteLabel(target));
    const bool targetIsConflict = diff::isConflictNote(target->title);
    m_conflictBadge->setVisible(targetIsConflict);
    m_controlsBar->show();

    const bool baseEncrypted = notelock::isEncrypted(base->body);
    const bool targetEncrypted = notelock::isEncrypted(target->body);
    if (baseEncrypted || targetEncrypted) {
        m_stats->setText(T("加密笔记无法直接对比正文"));
        m_diffView->setHtml(QStringLiteral("<p style=\"padding:24px;color:%1;\">%2</p>")
                                .arg(Theme::t().muted.name(), T("其中一篇笔记已加密，请先移除密码或解锁后再对比。").toHtmlEscaped()));
        m_stack->setCurrentWidget(m_diffView);
        m_openTargetBtn->setEnabled(true);
        m_appendBtn->setEnabled(false);
        m_replaceBtn->setEnabled(false);
        m_trashBtn->setVisible(targetIsConflict);
        return;
    }

    const diff::Result res = diff::compute(base->body, target->body);
    const Tokens &k = Theme::t();
    m_stats->setText(QStringLiteral("<span style=\"color:%1;\">-%2</span> &nbsp; <span style=\"color:%3;\">+%4</span> &nbsp; <span style=\"color:%5;\">=%6</span>")
                         .arg(k.danger.name())
                         .arg(res.removed)
                         .arg(k.ok.name())
                         .arg(res.added)
                         .arg(k.muted.name())
                         .arg(res.unchanged));

    const bool editable = !base->trashed();
    m_openTargetBtn->setEnabled(true);
    m_appendBtn->setEnabled(editable && !res.identical);
    m_replaceBtn->setEnabled(editable && !res.identical);
    m_trashBtn->setVisible(targetIsConflict || res.identical);

    if (res.identical) {
        m_stack->setCurrentWidget(m_identicalPage);
        return;
    }

    const QString delBg = rgba(k.danger, Theme::dark() ? 0.22 : 0.12);
    const QString addBg = rgba(k.ok, Theme::dark() ? 0.22 : 0.12);
    const QString strongDelBg = rgba(k.danger, Theme::dark() ? 0.45 : 0.30);
    const QString strongAddBg = rgba(k.ok, Theme::dark() ? 0.45 : 0.28);
    const QString emptyBg = rgba(k.muted, 0.06);
    const QString mono = Theme::mono(12.5).families().value(0, QStringLiteral("monospace"));

    QString html = QStringLiteral(
        "<html><head><style>"
        "body{margin:0;padding:0;background:%1;color:%2;font-family:'%3',monospace;font-size:12.5px;}"
        "table{width:100%;border-collapse:collapse;table-layout:fixed;}"
        "td{padding:2px 6px;vertical-align:top;white-space:pre-wrap;word-wrap:break-word;border-bottom:1px solid %4;}"
        "td.ln{width:36px;color:%5;text-align:right;padding-right:8px;user-select:none;}"
        "td.mk{width:16px;text-align:center;color:%5;user-select:none;}"
        "td.sep{width:1px;padding:0;background:%6;}"
        "td.del{background:%7;} td.add{background:%8;} td.emp{background:%9;}"
        "</style></head><body><table>")
                       .arg(k.surface.name(), k.ink.name(), mono, rgba(k.line, 0.45), k.muted.name(), k.line.name(), delBg, addBg, emptyBg);

    if (!m_unified) {
        for (const diff::Row &row : res.rows) {
            const bool isSame = row.left && row.right && row.left->kind == diff::Kind::Same && row.right->kind == diff::Kind::Same;
            if (m_changesOnly && isSame) continue;
            html += QStringLiteral("<tr>");
            if (row.left) {
                const QString cls = row.left->kind == diff::Kind::Del ? QStringLiteral("del") : QString();
                html += QStringLiteral("<td class=\"ln %1\">%2</td><td class=\"%1\">%3</td>")
                            .arg(cls, row.left->oldLine ? QString::number(row.left->oldLine) : QString(), renderSpans(*row.left, strongDelBg, strongAddBg));
            } else {
                html += QStringLiteral("<td class=\"ln emp\"></td><td class=\"emp\">&nbsp;</td>");
            }
            html += QStringLiteral("<td class=\"sep\"></td>");
            if (row.right) {
                const QString cls = row.right->kind == diff::Kind::Add ? QStringLiteral("add") : QString();
                html += QStringLiteral("<td class=\"ln %1\">%2</td><td class=\"%1\">%3</td>")
                            .arg(cls, row.right->newLine ? QString::number(row.right->newLine) : QString(), renderSpans(*row.right, strongDelBg, strongAddBg));
            } else {
                html += QStringLiteral("<td class=\"ln emp\"></td><td class=\"emp\">&nbsp;</td>");
            }
            html += QStringLiteral("</tr>");
        }
    } else {
        for (const diff::Line &line : res.unified) {
            if (m_changesOnly && line.kind == diff::Kind::Same) continue;
            const QString cls = line.kind == diff::Kind::Del ? QStringLiteral("del") : line.kind == diff::Kind::Add ? QStringLiteral("add") : QString();
            const QString mark = line.kind == diff::Kind::Del ? QStringLiteral("−") : line.kind == diff::Kind::Add ? QStringLiteral("+") : QStringLiteral("&nbsp;");
            html += QStringLiteral("<tr><td class=\"ln %1\">%2</td><td class=\"ln %1\">%3</td><td class=\"mk %1\">%4</td><td class=\"%1\">%5</td></tr>")
                        .arg(cls,
                             line.oldLine ? QString::number(line.oldLine) : QString(),
                             line.newLine ? QString::number(line.newLine) : QString(),
                             mark,
                             renderSpans(line, strongDelBg, strongAddBg));
        }
    }
    html += QStringLiteral("</table></body></html>");
    m_diffView->setHtml(html);
    m_stack->setCurrentWidget(m_diffView);
}

void NoteDiffDialog::appendRight() {
    if (!m_session) return;
    const Note *base = m_session->engine()->note(m_baseId);
    const Note *target = m_session->engine()->note(m_targetId);
    if (!base || !target || base->trashed()) return;
    const QString extra = target->body.trimmed();
    if (extra.isEmpty()) return;
    QString baseBody = base->body;
    while (!baseBody.isEmpty() && baseBody.back().isSpace()) baseBody.chop(1);
    NotePatch p;
    p.body = (baseBody.isEmpty() ? QString() : baseBody + QStringLiteral("\n\n---\n\n")) + extra + u'\n';
    m_session->engine()->updateNote(m_baseId, p);
    Toaster::show(T("已将右侧内容追加到当前笔记"));
    refreshDiff();
}

void NoteDiffDialog::replaceWithRight() {
    if (!m_session) return;
    const Note *base = m_session->engine()->note(m_baseId);
    const Note *target = m_session->engine()->note(m_targetId);
    if (!base || !target || base->trashed()) return;
    if (!dialogs::confirm(this, T("覆盖当前笔记"), T("确定要用对比笔记的正文覆盖当前笔记？覆盖后仍可通过历史版本找回。"), T("覆盖")))
        return;
    NotePatch p;
    p.body = target->body;
    m_session->engine()->updateNote(m_baseId, p);
    Toaster::show(T("已用右侧内容覆盖当前笔记"));
    refreshDiff();
}

void NoteDiffDialog::trashRight() {
    if (!m_session) return;
    const Note *target = m_session->engine()->note(m_targetId);
    if (!target) return;
    if (!dialogs::confirm(this, T("移到回收站"), T("将右侧对比副本「%1」移到回收站？").arg(noteLabel(target)), T("移到回收站")))
        return;
    NotePatch p;
    p.trashed = true;
    m_session->engine()->updateNote(m_targetId, p);
    Toaster::show(T("已将对比副本移到回收站"));
    accept();
}

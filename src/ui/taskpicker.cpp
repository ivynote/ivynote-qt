#include "ui/taskpicker.h"

#include "core/i18n.h"
#include "ui/theme.h"
#include "ui/widgets.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDateTime>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimeEdit>
#include <QVBoxLayout>

TaskDueDialog::TaskDueDialog(const std::optional<tasks::Due> &initialDue,
                             const std::optional<tasks::RepeatRule> &initialRepeat,
                             QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(T("日期与提醒"));
    setMinimumWidth(360);

    const bool zh = i18n::isZh();
    const QDateTime now = QDateTime::currentDateTime();

    auto *col = new QVBoxLayout(this);
    col->setContentsMargins(16, 16, 16, 16);
    col->setSpacing(12);

    // Quick-pick grid: clicking a chip sets due immediately (preserving current repeat selection).
    auto *quickLabel = mutedLabel(T("快捷日期"), this, 12);
    col->addWidget(quickLabel);
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(6);
    const auto quicks = tasks::quickDates(now, zh);
    for (int i = 0; i < quicks.size(); ++i) {
        const auto &q = quicks[i];
        auto *btn = new QPushButton(q.label, this);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setObjectName(QStringLiteral("quickDate_%1").arg(i));
        connect(btn, &QPushButton::clicked, this, [this, due = q.due] {
            m_due = due;
            m_repeat = m_repeatBox ? m_repeatBox->currentData().toString() : QString();
            m_action = Applied;
            accept();
        });
        grid->addWidget(btn, i / 3, i % 3);
    }
    col->addLayout(grid);

    // Custom fields: Date, optional Time, Repeat.
    auto *form = new QFormLayout;
    form->setContentsMargins(0, 4, 0, 0);
    form->setVerticalSpacing(10);

    m_date = new QDateEdit(initialDue && initialDue->valid() ? initialDue->date : now.date(), this);
    m_date->setObjectName(QStringLiteral("taskDueDate"));
    m_date->setCalendarPopup(true);
    m_date->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    form->addRow(T("日期"), m_date);

    auto *timeHost = new QWidget(this);
    auto *timeRow = new QHBoxLayout(timeHost);
    timeRow->setContentsMargins(0, 0, 0, 0);
    timeRow->setSpacing(8);
    m_useTime = new QCheckBox(T("指定时间"), timeHost);
    m_useTime->setObjectName(QStringLiteral("taskUseTime"));
    m_time = new QTimeEdit(initialDue && !initialDue->allDay() ? initialDue->time : QTime(9, 0), timeHost);
    m_time->setObjectName(QStringLiteral("taskDueTime"));
    m_time->setDisplayFormat(QStringLiteral("HH:mm"));
    const bool hasTime = initialDue && !initialDue->allDay();
    m_useTime->setChecked(hasTime);
    m_time->setEnabled(hasTime);
    connect(m_useTime, &QCheckBox::toggled, m_time, &QWidget::setEnabled);
    timeRow->addWidget(m_useTime);
    timeRow->addWidget(m_time, 1);
    form->addRow(T("时间"), timeHost);

    m_repeatBox = new QComboBox(this);
    m_repeatBox->setObjectName(QStringLiteral("taskRepeatBox"));
    for (const auto &ch : tasks::repeatChoices(zh)) m_repeatBox->addItem(ch.label, ch.raw);
    if (initialRepeat) {
        int idx = m_repeatBox->findData(initialRepeat->raw);
        if (idx < 0) {
            m_repeatBox->addItem(tasks::describeRepeat(*initialRepeat, zh), initialRepeat->raw);
            idx = m_repeatBox->count() - 1;
        }
        m_repeatBox->setCurrentIndex(idx);
    }
    form->addRow(T("重复"), m_repeatBox);
    col->addLayout(form);

    auto *hint = mutedLabel(T("不指定时间时，将在当天 09:00 提醒。"), this, 12);
    hint->setWordWrap(true);
    col->addWidget(hint);

    // Bottom buttons.
    auto *btns = new QHBoxLayout;
    btns->setSpacing(8);
    if (initialDue && initialDue->valid()) {
        auto *removeBtn = new QPushButton(T("移除日期"), this);
        removeBtn->setObjectName(QStringLiteral("taskRemoveDue"));
        connect(removeBtn, &QPushButton::clicked, this, [this] {
            m_due = std::nullopt;
            m_repeat.clear();
            m_action = Removed;
            accept();
        });
        btns->addWidget(removeBtn);
    }
    btns->addStretch(1);
    auto *cancelBtn = new QPushButton(T("取消"), this);
    auto *okBtn = new QPushButton(T("确定"), this);
    okBtn->setObjectName(QStringLiteral("taskDueOk"));
    okBtn->setDefault(true);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(okBtn, &QPushButton::clicked, this, &TaskDueDialog::applyFromFields);
    btns->addWidget(cancelBtn);
    btns->addWidget(okBtn);
    col->addLayout(btns);
}

void TaskDueDialog::applyFromFields() {
    const QDate d = m_date->date();
    if (!d.isValid()) {
        reject();
        return;
    }
    m_due = tasks::Due{d, m_useTime->isChecked() ? m_time->time() : QTime()};
    m_repeat = m_repeatBox->currentData().toString();
    m_action = Applied;
    accept();
}

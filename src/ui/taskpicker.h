// Date & repeat picker for tasks, aligned with web/Tauri's openDuePicker.
#pragma once

#include "core/tasks.h"

#include <QDialog>
#include <optional>

class QCheckBox;
class QComboBox;
class QDateEdit;
class QTimeEdit;

class TaskDueDialog : public QDialog {
    Q_OBJECT
public:
    enum Action { Canceled, Applied, Removed };

    TaskDueDialog(const std::optional<tasks::Due> &initialDue,
                  const std::optional<tasks::RepeatRule> &initialRepeat,
                  QWidget *parent = nullptr);

    Action action() const { return m_action; }
    std::optional<tasks::Due> selectedDue() const { return m_due; }
    QString selectedRepeat() const { return m_repeat; }

private:
    void applyFromFields();

    Action m_action = Canceled;
    std::optional<tasks::Due> m_due;
    QString m_repeat;

    QDateEdit *m_date = nullptr;
    QCheckBox *m_useTime = nullptr;
    QTimeEdit *m_time = nullptr;
    QComboBox *m_repeatBox = nullptr;
};

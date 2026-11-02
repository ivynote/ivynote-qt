// Search and command palette (Ctrl+K; ">" switches to commands). Local search
// only (guide 11.5): it works offline and in local vaults.
#pragma once

#include <QDialog>
#include <functional>

class QLineEdit;
class QListWidget;
class SyncEngine;

struct PaletteCommand {
    QString title;
    QString shortcut;
    QString icon;
    std::function<void()> run;
};

class CommandPalette : public QDialog {
    Q_OBJECT
public:
    CommandPalette(SyncEngine *engine, QList<PaletteCommand> commands, QWidget *parent);
    void open(const QString &initial);

signals:
    void openNote(const QString &id);

protected:
    bool eventFilter(QObject *o, QEvent *e) override;
    void paintEvent(QPaintEvent *) override;

private:
    void update(const QString &q);
    void activate();

    SyncEngine *m_engine;
    QList<PaletteCommand> m_commands;
    QLineEdit *m_input;
    QListWidget *m_list;
};

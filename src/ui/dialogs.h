// Small dialogs and popups.
#pragma once

#include <QByteArray>
#include <QPoint>
#include <QString>

class QWidget;
class SyncEngine;

namespace dialogs {

// Choose a notebook ("" = root). Returns false when canceled.
bool pickFolder(SyncEngine *engine, QWidget *parent, const QString &title, const QString &current, QString *out,
                const QString &excludeTree = {});

// Tag picker for a note, anchored at a screen position.
void tagPicker(SyncEngine *engine, const QString &noteId, const QPoint &global, QWidget *parent);

void showRecoveryKey(QWidget *parent, const QString &vaultName, const QString &key, bool justCreated);
// Asks for a recovery key; empty when canceled.
QByteArray askRecoveryKey(QWidget *parent, const QString &vaultName, bool wrongBefore);

// A Yes/No question with a destructive Yes.
bool confirm(QWidget *parent, const QString &title, const QString &text, const QString &yes);

} // namespace dialogs

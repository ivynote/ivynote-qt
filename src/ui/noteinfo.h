// Note info: when it was made, where it lives, how long it is, how it is
// shared and which devices edited it. Counts come from the text on this
// device; sharing and history are asked of the server when there is one.
#pragma once

#include <QDialog>
#include <QPointer>

class Session;
class QLabel;
class QVBoxLayout;

class NoteInfoDialog : public QDialog {
    Q_OBJECT
public:
    NoteInfoDialog(Session *session, const QString &noteId, QWidget *parent = nullptr);

signals:
    void shareRequested();
    void historyRequested();

private:
    void loadRemote();

    QPointer<Session> m_session;
    QString m_noteId;
    QLabel *m_share = nullptr;
    QLabel *m_revisions = nullptr;
    QVBoxLayout *m_devices = nullptr;
};

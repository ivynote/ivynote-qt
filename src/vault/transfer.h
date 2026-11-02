// Moving notes in and out of a vault. Everything goes through the engine's
// normal operations, so imported notes sync like hand-written ones (and an
// import into a synced vault also works offline: files queue for upload).
//
// This is also how a local vault's notes reach a synced one: create the
// synced vault, then import the local vault into it.
#pragma once

#include "vault/vaults.h"

#include <QObject>
#include <QStringList>

class Session;

class TransferJob : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void start() = 0;
    void cancel() { m_canceled = true; }

signals:
    void progress(int done, int total, const QString &what);
    void finished(bool ok, const QString &summary);

protected:
    bool m_canceled = false;
};

namespace transfer {

// Notes as Markdown files in a folder tree, with YAML front matter and an
// _attachments/ folder; links between notes and to files become relative.
TransferJob *exportMarkdown(Session *session, const QString &dir, QObject *parent = nullptr);

// Markdown files or folders; sub-folders become notebooks, front matter
// (title, created, updated, tags, pinned) is kept, linked local files are
// attached and links between imported notes are rewired.
TransferJob *importMarkdown(Session *session, const QStringList &paths, const QString &targetFolderId,
                            QObject *parent = nullptr);

// Notebooks, tags, notes and their files from another vault on this device.
// Notes in its trash are left behind. Ids are new, so importing twice
// duplicates rather than overwrites.
TransferJob *importVault(Session *target, const VaultInfo &source, QObject *parent = nullptr);

} // namespace transfer

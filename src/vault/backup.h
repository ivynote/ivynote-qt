// Incremental local Markdown backup for local-only vaults.
//
// Only local vaults (VaultInfo::Local) participate: synced vaults already back
// up to their server. Backups write standard Markdown files + YAML front matter
// + _attachments/ compatible with transfer::importMarkdown(), tracking state in
// .ivynote-backup.json so only changed notes and attachments are written and
// untracked user files in the backup folder are never touched.
#pragma once

#include "vault/transfer.h"
#include "vault/vaults.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

class Session;

namespace backup {

inline constexpr qint64 kLowSpaceWarningBytes = 512LL * 1024 * 1024; // 512 MiB
inline constexpr qint64 kCriticalSpaceBytes = 100LL * 1024 * 1024;   // 100 MiB
inline constexpr int kDefaultIntervalMinutes = 30;
inline constexpr int kMaxHistoryEntries = 30;
inline constexpr const char *kManifestFileName = ".ivynote-backup.json";

struct VolumeCandidate {
    QString rootPath;
    QString displayName;
    QString fileSystemType;
    QString candidateBaseDir; // writable IvynoteBackups base directory on this volume
    qint64 bytesTotal = 0;
    qint64 bytesAvailable = 0;
    bool valid = false;
    bool ready = false;
    bool readOnly = false;
    bool writable = false;
};

struct SpaceCheck {
    bool valid = false;
    QString mountRoot;
    qint64 bytesTotal = 0;
    qint64 bytesAvailable = 0;
    qint64 recommendedMinBytes = kLowSpaceWarningBytes;
    qint64 criticalMinBytes = kCriticalSpaceBytes;
    bool lowSpace = false;
    bool criticalSpace = false;
};

struct Config {
    bool enabled = true;        // enabled by default for local vaults
    bool configured = false;    // becomes true after first-run setup or explicit save
    QString directory;          // backup folder for this local vault
    int intervalMinutes = kDefaultIntervalMinutes;
};

struct RunRecord {
    qint64 timestamp = 0;
    bool ok = false;
    bool manual = false;
    int totalNotes = 0;
    int writtenNotes = 0;
    int unchangedNotes = 0;
    int removedNotes = 0;
    int totalAttachments = 0;
    int writtenAttachments = 0;
    int unchangedAttachments = 0;
    int removedAttachments = 0;
    int missingAttachments = 0;
    qint64 durationMs = 0;
    QString summary;
    QString error;
};

struct ManifestSummary {
    bool exists = false;
    qint64 lastBackupAt = 0;
    int totalNotes = 0;
    int totalAttachments = 0;
    QList<RunRecord> history;
};

// --- Storage scanning & validation ---

QString safeVaultFolderName(const VaultInfo &vault);
bool isVirtualFileSystem(const QString &fsType, const QString &rootPath);
VolumeCandidate pickBestVolume(const QList<VolumeCandidate> &volumes, qint64 minSpaceBytes = kLowSpaceWarningBytes);
QList<VolumeCandidate> scanVolumes();
QString suggestDefaultDirectory(const VaultInfo &vault);

qint64 estimateVaultBytes(const VaultInfo &vault);
SpaceCheck checkSpace(const QString &dir, qint64 estimatedVaultBytes = 0);
bool isPathInsideVault(const QString &dir, const VaultInfo &vault);
bool isDirectoryWritable(const QString &dir);

// --- Per-vault config & manifest ---

Config loadConfig(const VaultInfo &vault);
void saveConfig(const QString &vaultId, const Config &cfg);
void removeConfig(const QString &vaultId);
ManifestSummary readManifestSummary(const QString &dir);

// --- Incremental export ---

// Asynchronous incremental export job (local vaults only). Writes changed/new
// notes and attachments atomically via QSaveFile, removes previously-tracked
// stale files, and leaves untracked user files untouched.
TransferJob *exportMarkdownIncremental(Session *session, const QString &dir, bool manual = true,
                                       RunRecord *outRecord = nullptr, QObject *parent = nullptr);

// Synchronous incremental export for quick flush when closing a local vault.
RunRecord runIncrementalSync(Session *session, const QString &dir, bool manual = false);

} // namespace backup

// Coordinates scheduled and on-demand incremental backups for the active local vault.
class LocalBackupManager : public QObject {
    Q_OBJECT
public:
    explicit LocalBackupManager(QObject *parent = nullptr);
    ~LocalBackupManager() override;

    void setSession(Session *session);
    bool isActive() const; // true iff a local vault is currently attached
    bool isRunning() const { return m_job != nullptr; }
    backup::Config config() const { return m_config; }
    backup::ManifestSummary manifestSummary() const;
    backup::SpaceCheck currentSpace() const;

    void updateConfig(const backup::Config &cfg);
    void runNow(bool manual = true);
    void runIfDirtyOnClose();

signals:
    void stateChanged();
    void backupStarted(bool manual);
    void backupProgress(int done, int total, const QString &what);
    void backupFinished(const backup::RunRecord &record);
    void lowSpaceDetected(const backup::SpaceCheck &space);
    void needsInitialSetup();

private:
    void resetTimer();
    void onScheduledTick();
    void onVaultDataChanged();

    QPointer<Session> m_session;
    backup::Config m_config;
    QPointer<TransferJob> m_job;
    backup::RunRecord m_pendingRecord;
    QTimer m_timer;
    bool m_dirty = false;
    bool m_lowSpaceWarned = false;
    bool m_setupSignaled = false;
};

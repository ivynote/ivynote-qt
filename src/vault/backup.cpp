#include "vault/backup.h"

#include "core/format.h"
#include "core/i18n.h"
#include "core/markdown.h"
#include "sync/engine.h"
#include "vault/files.h"
#include "vault/session.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QUuid>
#include <algorithm>
#include <deque>

namespace {

using namespace backup;

QString safeName(QString s, const QString &fallback) {
    static const QRegularExpression bad(QStringLiteral(R"([\\/:*?"<>|\x00-\x1f])"));
    s.replace(bad, QStringLiteral("_"));
    s = s.trimmed();
    while (s.endsWith(u'.')) s.chop(1);
    if (s.isEmpty()) s = fallback;
    return s.left(120);
}

QString uniqueIn(QSet<QString> &taken, const QString &base, const QString &ext) {
    QString name = base + ext;
    for (int i = 2; taken.contains(name.toLower()); ++i)
        name = QStringLiteral("%1 (%2)%3").arg(base).arg(i).arg(ext);
    taken.insert(name.toLower());
    return name;
}

QString relLink(const QString &fromDir, const QString &toPath) {
    const QString rel = QDir(fromDir).relativeFilePath(toPath);
    QString out;
    out.reserve(rel.size());
    for (QChar c : rel) {
        if (c == u' ') out += QStringLiteral("%20");
        else if (c == u'(') out += QStringLiteral("%28");
        else if (c == u')') out += QStringLiteral("%29");
        else out += c;
    }
    return out;
}

QString isoTime(qint64 ms) {
    return QDateTime::fromMSecsSinceEpoch(ms)
        .toOffsetFromUtc(QDateTime::currentDateTime().offsetFromUtc())
        .toString(Qt::ISODate);
}

QString yamlString(const QString &s) {
    QString e = s;
    e.replace(u'\\', QStringLiteral("\\\\")).replace(u'"', QStringLiteral("\\\""));
    return u'"' + e + u'"';
}

QString cleanDirPath(const QString &dir) {
    if (dir.trimmed().isEmpty()) return {};
    return QDir::cleanPath(QDir::fromNativeSeparators(dir.trimmed()));
}

QString nearestExistingAncestor(const QString &path) {
    QString cur = cleanDirPath(path);
    if (cur.isEmpty()) return {};
    QFileInfo fi(cur);
    while (!fi.exists()) {
        QDir parent = fi.dir();
        const QString next = QDir::cleanPath(parent.absolutePath());
        if (next == cur || next.isEmpty()) break;
        cur = next;
        fi = QFileInfo(cur);
    }
    return fi.exists() ? fi.absoluteFilePath() : QString();
}

bool probeDirWritable(const QString &dir) {
    const QString existing = nearestExistingAncestor(dir);
    if (existing.isEmpty()) return false;
    QFileInfo fi(existing);
    if (!fi.isDir() || !fi.isWritable()) return false;
    // Actually test creating & removing a hidden zero-byte probe file to handle
    // ACLs, read-only mounts, and Windows/Linux permission edge cases accurately.
    const QString probe = QDir(existing).filePath(QStringLiteral(".ivynote-probe-") + QUuid::createUuid().toString(QUuid::Id128));
    QFile f(probe);
    if (!f.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return false;
    f.close();
    f.remove();
    return true;
}

bool isSafeRelativePath(const QString &rel) {
    if (rel.isEmpty() || QDir::isAbsolutePath(rel)) return false;
    const QString norm = QDir::cleanPath(QDir::fromNativeSeparators(rel));
    if (norm.isEmpty() || norm == QLatin1String(".") || norm == QLatin1String("..")) return false;
    if (norm.startsWith(QLatin1String("../")) || norm.contains(QLatin1String("/../")) || norm.endsWith(QLatin1String("/.."))) return false;
    if (QDir::isAbsolutePath(norm)) return false;
    return true;
}

QJsonObject recordToJson(const RunRecord &r) {
    return QJsonObject{
        {QStringLiteral("timestamp"), double(r.timestamp)},
        {QStringLiteral("ok"), r.ok},
        {QStringLiteral("manual"), r.manual},
        {QStringLiteral("totalNotes"), r.totalNotes},
        {QStringLiteral("writtenNotes"), r.writtenNotes},
        {QStringLiteral("unchangedNotes"), r.unchangedNotes},
        {QStringLiteral("removedNotes"), r.removedNotes},
        {QStringLiteral("totalAttachments"), r.totalAttachments},
        {QStringLiteral("writtenAttachments"), r.writtenAttachments},
        {QStringLiteral("unchangedAttachments"), r.unchangedAttachments},
        {QStringLiteral("removedAttachments"), r.removedAttachments},
        {QStringLiteral("missingAttachments"), r.missingAttachments},
        {QStringLiteral("durationMs"), double(r.durationMs)},
        {QStringLiteral("summary"), r.summary},
        {QStringLiteral("error"), r.error},
    };
}

RunRecord recordFromJson(const QJsonObject &o) {
    RunRecord r;
    r.timestamp = qint64(o.value(QLatin1String("timestamp")).toDouble());
    r.ok = o.value(QLatin1String("ok")).toBool();
    r.manual = o.value(QLatin1String("manual")).toBool();
    r.totalNotes = o.value(QLatin1String("totalNotes")).toInt();
    r.writtenNotes = o.value(QLatin1String("writtenNotes")).toInt();
    r.unchangedNotes = o.value(QLatin1String("unchangedNotes")).toInt();
    r.removedNotes = o.value(QLatin1String("removedNotes")).toInt();
    r.totalAttachments = o.value(QLatin1String("totalAttachments")).toInt();
    r.writtenAttachments = o.value(QLatin1String("writtenAttachments")).toInt();
    r.unchangedAttachments = o.value(QLatin1String("unchangedAttachments")).toInt();
    r.removedAttachments = o.value(QLatin1String("removedAttachments")).toInt();
    r.missingAttachments = o.value(QLatin1String("missingAttachments")).toInt();
    r.durationMs = qint64(o.value(QLatin1String("durationMs")).toDouble());
    r.summary = o.value(QLatin1String("summary")).toString();
    r.error = o.value(QLatin1String("error")).toString();
    return r;
}

struct TrackedEntry {
    QString path; // relative path inside backup dir
    QString hash; // hex sha256
    qint64 size = 0;
};

struct ManifestData {
    QString vaultId;
    QString vaultName;
    qint64 lastBackupAt = 0;
    QHash<QString, TrackedEntry> notes;
    QHash<QString, TrackedEntry> attachments;
    QList<RunRecord> history;
};

ManifestData loadManifest(const QString &dir) {
    ManifestData m;
    QFile f(QDir(dir).filePath(QLatin1String(kManifestFileName)));
    if (!f.open(QIODevice::ReadOnly)) return m;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return m;
    const QJsonObject root = doc.object();
    m.vaultId = root.value(QLatin1String("vaultId")).toString();
    m.vaultName = root.value(QLatin1String("vaultName")).toString();
    m.lastBackupAt = qint64(root.value(QLatin1String("lastBackupAt")).toDouble());
    const QJsonObject notesObj = root.value(QLatin1String("notes")).toObject();
    for (auto it = notesObj.constBegin(); it != notesObj.constEnd(); ++it) {
        const QJsonObject eo = it.value().toObject();
        TrackedEntry e;
        e.path = eo.value(QLatin1String("path")).toString();
        e.hash = eo.value(QLatin1String("hash")).toString();
        e.size = qint64(eo.value(QLatin1String("size")).toDouble());
        if (isSafeRelativePath(e.path)) m.notes.insert(it.key(), e);
    }
    const QJsonObject attObj = root.value(QLatin1String("attachments")).toObject();
    for (auto it = attObj.constBegin(); it != attObj.constEnd(); ++it) {
        const QJsonObject eo = it.value().toObject();
        TrackedEntry e;
        e.path = eo.value(QLatin1String("path")).toString();
        e.hash = eo.value(QLatin1String("hash")).toString();
        e.size = qint64(eo.value(QLatin1String("size")).toDouble());
        if (isSafeRelativePath(e.path)) m.attachments.insert(it.key(), e);
    }
    for (const auto &v : root.value(QLatin1String("history")).toArray()) {
        if (v.isObject()) m.history << recordFromJson(v.toObject());
    }
    return m;
}

bool saveManifest(const QString &dir, const ManifestData &m) {
    QJsonObject notesObj;
    for (auto it = m.notes.constBegin(); it != m.notes.constEnd(); ++it) {
        notesObj.insert(it.key(), QJsonObject{
                                      {QStringLiteral("path"), it->path},
                                      {QStringLiteral("hash"), it->hash},
                                      {QStringLiteral("size"), double(it->size)},
                                  });
    }
    QJsonObject attObj;
    for (auto it = m.attachments.constBegin(); it != m.attachments.constEnd(); ++it) {
        attObj.insert(it.key(), QJsonObject{
                                    {QStringLiteral("path"), it->path},
                                    {QStringLiteral("hash"), it->hash},
                                    {QStringLiteral("size"), double(it->size)},
                                });
    }
    QJsonArray hist;
    const int n = std::min<int>(int(m.history.size()), kMaxHistoryEntries);
    for (int i = 0; i < n; ++i) hist.append(recordToJson(m.history[i]));

    const QJsonObject root{
        {QStringLiteral("version"), 1},
        {QStringLiteral("vaultId"), m.vaultId},
        {QStringLiteral("vaultName"), m.vaultName},
        {QStringLiteral("lastBackupAt"), double(m.lastBackupAt)},
        {QStringLiteral("notes"), notesObj},
        {QStringLiteral("attachments"), attObj},
        {QStringLiteral("history"), hist},
    };
    QSaveFile f(QDir(dir).filePath(QLatin1String(kManifestFileName)));
    if (!f.open(QIODevice::WriteOnly)) return false;
    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (f.write(payload) != payload.size()) return false;
    return f.commit();
}

void removeEmptyAncestors(const QString &rootDir, const QString &filePath) {
    const QString rootClean = cleanDirPath(rootDir);
    QDir cur = QFileInfo(filePath).dir();
    while (true) {
        const QString curClean = cleanDirPath(cur.absolutePath());
        if (curClean.isEmpty() || curClean == rootClean || !curClean.startsWith(rootClean + u'/')) break;
        if (!cur.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty()) break;
        const QString dirName = cur.dirName();
        if (!cur.cdUp() || !cur.rmdir(dirName)) break;
    }
}

QString formatRunSummary(const RunRecord &r) {
    const bool zh = i18n::isZh();
    if (!r.ok) return r.error.isEmpty() ? T("备份失败") : r.error;
    if (r.writtenNotes == 0 && r.removedNotes == 0 && r.writtenAttachments == 0 && r.removedAttachments == 0 && r.missingAttachments == 0) {
        return zh ? QStringLiteral("无变化（共 %1 篇笔记）").arg(r.totalNotes)
                  : QStringLiteral("No changes (%1 notes)").arg(r.totalNotes);
    }
    QStringList parts;
    if (r.writtenNotes > 0)
        parts << (zh ? QStringLiteral("更新 %1 篇笔记").arg(r.writtenNotes) : QStringLiteral("%1 notes updated").arg(r.writtenNotes));
    if (r.unchangedNotes > 0)
        parts << (zh ? QStringLiteral("跳过 %1 篇").arg(r.unchangedNotes) : QStringLiteral("%1 unchanged").arg(r.unchangedNotes));
    if (r.removedNotes > 0)
        parts << (zh ? QStringLiteral("清理 %1 篇").arg(r.removedNotes) : QStringLiteral("%1 removed").arg(r.removedNotes));
    if (r.writtenAttachments > 0)
        parts << (zh ? QStringLiteral("更新 %1 个附件").arg(r.writtenAttachments) : QStringLiteral("%1 attachments updated").arg(r.writtenAttachments));
    if (r.missingAttachments > 0)
        parts << (zh ? QStringLiteral("%1 个附件缺失").arg(r.missingAttachments) : QStringLiteral("%1 attachments missing").arg(r.missingAttachments));
    return parts.join(zh ? QStringLiteral("，") : QStringLiteral(", "));
}

struct PlannedNote {
    QString id;
    QString relPath;
    QByteArray content;
    QString hash;
};

struct PlannedAttachment {
    QString id;
    QString relPath;
    qint64 expectedSize = 0;
    QString expectedHash;
};

class IncrementalBackupJob : public TransferJob {
public:
    IncrementalBackupJob(Session *session, QString dir, bool manual, RunRecord *outRecord, QObject *parent)
        : TransferJob(parent), m_session(session), m_dir(cleanDirPath(dir)), m_manual(manual), m_outRecord(outRecord) {
        m_record.manual = manual;
    }

    void start() override {
        m_elapsed.start();
        if (!prepare()) {
            finish(false);
            return;
        }
        processNoteBatch();
    }

    RunRecord runSync() {
        m_elapsed.start();
        if (!prepare()) {
            finishSync(false);
            return m_record;
        }
        for (const PlannedNote &pn : std::as_const(m_notesToWrite)) {
            if (!writeNote(pn)) {
                finishSync(false);
                return m_record;
            }
        }
        for (const PlannedAttachment &pa : std::as_const(m_attsToWrite)) {
            QByteArray data;
            bool ok = false;
            if (pa.id.startsWith(QLatin1String("local-"))) ok = m_session->files()->readUpload(pa.id, &data);
            else ok = m_session->files()->readFile(pa.id, &data, false);
            if (!handleAttachmentData(pa, ok, data)) {
                finishSync(false);
                return m_record;
            }
        }
        finalizeSuccess();
        finishSync(true);
        return m_record;
    }

private:
    bool prepare() {
        m_record.timestamp = QDateTime::currentMSecsSinceEpoch();
        if (!m_session || !m_session->engine()) {
            m_record.error = T("没有打开的笔记库。");
            return false;
        }
        if (m_dir.isEmpty()) {
            m_record.error = T("未设置备份目录。");
            return false;
        }
        if (isPathInsideVault(m_dir, m_session->info())) {
            m_record.error = T("备份目录不能位于笔记库数据目录内，请重新选择。");
            return false;
        }
        const SpaceCheck space = checkSpace(m_dir, estimateVaultBytes(m_session->info()));
        if (space.valid && space.criticalSpace) {
            m_record.error = (i18n::isZh()
                                  ? QStringLiteral("目标磁盘可用空间严重不足（仅剩 %1），已取消本次备份，请更换备份目录。")
                                  : QStringLiteral("Target disk is critically low on space (%1 free). Backup canceled; please choose another folder."))
                                 .arg(fmt::bytes(space.bytesAvailable));
            return false;
        }
        if (!QDir().mkpath(m_dir) || !probeDirWritable(m_dir)) {
            m_record.error = (i18n::isZh()
                                  ? QStringLiteral("无法写入备份目录：%1")
                                  : QStringLiteral("Cannot write to backup folder: %1"))
                                 .arg(QDir::toNativeSeparators(m_dir));
            return false;
        }

        m_prev = loadManifest(m_dir);
        // Only trust tracked paths from the same vault (avoid removing unrelated files).
        if (!m_prev.vaultId.isEmpty() && m_prev.vaultId != m_session->info().id) {
            m_prev.notes.clear();
            m_prev.attachments.clear();
        }
        m_next.vaultId = m_session->info().id;
        m_next.vaultName = m_session->info().name;
        m_next.history = m_prev.history;

        SyncEngine *e = m_session->engine();
        // Deterministic folder ordering: (createdAt ASC, id ASC) so duplicate folder
        // names always receive stable suffixes across incremental runs.
        QList<const Folder *> sortedFolders;
        for (const Folder &f : e->folders()) {
            if (!f.deleted) sortedFolders << &f;
        }
        std::sort(sortedFolders.begin(), sortedFolders.end(), [](const Folder *a, const Folder *b) {
            return a->createdAt != b->createdAt ? a->createdAt < b->createdAt : a->id < b->id;
        });

        QHash<QString, QString> folderRelDir;
        QHash<QString, QSet<QString>> taken; // relDir -> lowercase names used
        taken[QString()].insert(QStringLiteral("_attachments"));
        taken[QString()].insert(QString::fromLatin1(kManifestFileName).toLower());

        QSet<QString> visiting;
        std::function<QString(const QString &)> relDirOf = [&](const QString &fid) -> QString {
            if (fid.isEmpty() || !e->folders().contains(fid) || e->folders()[fid].deleted) return {};
            if (folderRelDir.contains(fid)) return folderRelDir.value(fid);
            if (visiting.contains(fid)) return {}; // guard against malformed cycle
            visiting.insert(fid);
            const Folder &f = e->folders()[fid];
            const QString parent = f.parentId == fid ? QString() : relDirOf(f.parentId);
            const QString name = uniqueIn(taken[parent], safeName(f.name, T("笔记本")), QString());
            const QString rel = parent.isEmpty() ? name : parent + u'/' + name;
            folderRelDir.insert(fid, rel);
            visiting.remove(fid);
            return rel;
        };
        for (const Folder *f : sortedFolders) relDirOf(f->id);

        // Deterministic note ordering: (createdAt ASC, id ASC).
        QList<const Note *> notes;
        for (const Note &n : e->notes()) {
            if (!n.deleted && !n.trashed()) notes << &n;
        }
        std::sort(notes.begin(), notes.end(), [](const Note *a, const Note *b) {
            return a->createdAt != b->createdAt ? a->createdAt < b->createdAt : a->id < b->id;
        });

        QHash<QString, QString> noteRelPath;
        for (const Note *n : notes) {
            const QString dirRel = relDirOf(n->folderId);
            const QString file = uniqueIn(taken[dirRel], safeName(n->title, T("无标题")), QStringLiteral(".md"));
            noteRelPath.insert(n->id, dirRel.isEmpty() ? file : dirRel + u'/' + file);
        }

        // Referenced attachments, sorted deterministically by attachment ID.
        QSet<QString> refSet;
        for (const Note *n : notes) {
            for (const QString &id : md::resourceRefs(n->body)) refSet.insert(id);
        }
        QStringList sortedAttIds(refSet.cbegin(), refSet.cend());
        std::sort(sortedAttIds.begin(), sortedAttIds.end());

        QSet<QString> takenFiles;
        QHash<QString, QString> attRelPath;
        for (const QString &id : sortedAttIds) {
            const QString name = uniqueIn(takenFiles, safeName(e->attachmentName(id), id), QString());
            attRelPath.insert(id, QStringLiteral("_attachments/") + name);
        }

        static const QRegularExpression resRe(QStringLiteral(R"(ivy://res/([A-Za-z0-9_-]{8,64}))"));
        static const QRegularExpression noteRefRe(QStringLiteral(R"(ivy://note/([A-Za-z0-9_-]{8,64}))"));

        m_record.totalNotes = int(notes.size());
        m_record.totalAttachments = int(sortedAttIds.size());

        for (const Note *n : notes) {
            const QString rel = noteRelPath.value(n->id);
            const QString abs = QDir(m_dir).filePath(rel);
            const QString absDir = QFileInfo(abs).absolutePath();
            QString body = n->body;
            QString out;
            qsizetype last = 0;
            for (auto it = resRe.globalMatch(body); it.hasNext();) {
                const auto m = it.next();
                out += body.mid(last, m.capturedStart() - last);
                out += attRelPath.contains(m.captured(1))
                           ? relLink(absDir, QDir(m_dir).filePath(attRelPath.value(m.captured(1))))
                           : m.captured(0);
                last = m.capturedEnd();
            }
            out += body.mid(last);
            body = out;
            out.clear();
            last = 0;
            for (auto it = noteRefRe.globalMatch(body); it.hasNext();) {
                const auto m = it.next();
                out += body.mid(last, m.capturedStart() - last);
                out += noteRelPath.contains(m.captured(1))
                           ? relLink(absDir, QDir(m_dir).filePath(noteRelPath.value(m.captured(1))))
                           : m.captured(0);
                last = m.capturedEnd();
            }
            out += body.mid(last);

            QStringList tagNames;
            for (const QString &t : n->tags) {
                if (e->tags().contains(t) && !e->tags()[t].deleted) tagNames << yamlString(e->tags()[t].name);
            }
            QString front = QStringLiteral("---\ntitle: %1\ncreated: %2\nupdated: %3\n")
                                .arg(yamlString(n->title), isoTime(n->createdAt), isoTime(n->updatedAt));
            if (!tagNames.isEmpty()) front += QStringLiteral("tags: [%1]\n").arg(tagNames.join(QStringLiteral(", ")));
            if (n->pinned) front += QStringLiteral("pinned: true\n");
            front += QStringLiteral("---\n\n");

            const QByteArray bytes = (front + out).toUtf8();
            const QString hash = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
            m_next.notes.insert(n->id, TrackedEntry{rel, hash, bytes.size()});

            const auto prevIt = m_prev.notes.constFind(n->id);
            const QFileInfo existingInfo(abs);
            const bool unchanged = prevIt != m_prev.notes.constEnd() &&
                                   prevIt->path == rel &&
                                   prevIt->hash == hash &&
                                   prevIt->size == bytes.size() &&
                                   existingInfo.exists() &&
                                   existingInfo.size() == bytes.size();
            if (unchanged) {
                m_record.unchangedNotes++;
            } else {
                m_notesToWrite.push_back(PlannedNote{n->id, rel, bytes, hash});
            }
        }

        for (const QString &id : sortedAttIds) {
            const QString rel = attRelPath.value(id);
            const QString abs = QDir(m_dir).filePath(rel);
            const Attachment meta = e->attachments().value(id);
            const auto prevIt = m_prev.attachments.constFind(id);
            const QFileInfo existingInfo(abs);
            const bool unchanged = prevIt != m_prev.attachments.constEnd() &&
                                   prevIt->path == rel &&
                                   existingInfo.exists() &&
                                   existingInfo.size() == prevIt->size &&
                                   (meta.size <= 0 || meta.size == prevIt->size) &&
                                   (meta.sha256.isEmpty() || prevIt->hash == meta.sha256);
            if (unchanged) {
                m_next.attachments.insert(id, *prevIt);
                m_record.unchangedAttachments++;
            } else {
                m_attsToWrite.push_back(PlannedAttachment{id, rel, meta.size, meta.sha256});
            }
        }

        m_totalWork = int(m_notesToWrite.size() + m_attsToWrite.size());
        return true;
    }

    bool writeNote(const PlannedNote &pn) {
        const QString abs = QDir(m_dir).filePath(pn.relPath);
        if (!QDir().mkpath(QFileInfo(abs).absolutePath())) {
            m_record.error = (i18n::isZh() ? QStringLiteral("无法创建目录：%1") : QStringLiteral("Cannot create folder: %1"))
                                 .arg(QFileInfo(abs).absolutePath());
            return false;
        }
        QSaveFile f(abs);
        if (!f.open(QIODevice::WriteOnly) || f.write(pn.content) != pn.content.size() || !f.commit()) {
            m_record.error = (i18n::isZh() ? QStringLiteral("写入笔记失败：%1") : QStringLiteral("Failed to write note: %1"))
                                 .arg(pn.relPath);
            return false;
        }
        m_record.writtenNotes++;
        return true;
    }

    bool handleAttachmentData(const PlannedAttachment &pa, bool ok, const QByteArray &data) {
        if (!ok) {
            m_record.missingAttachments++;
            return true;
        }
        const QString abs = QDir(m_dir).filePath(pa.relPath);
        if (!QDir().mkpath(QFileInfo(abs).absolutePath())) {
            m_record.error = (i18n::isZh() ? QStringLiteral("无法创建附件目录：%1") : QStringLiteral("Cannot create attachment folder: %1"))
                                 .arg(QFileInfo(abs).absolutePath());
            return false;
        }
        QSaveFile f(abs);
        if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit()) {
            m_record.error = (i18n::isZh() ? QStringLiteral("写入附件失败：%1") : QStringLiteral("Failed to write attachment: %1"))
                                 .arg(pa.relPath);
            return false;
        }
        const QString hash = !pa.expectedHash.isEmpty()
                                 ? pa.expectedHash
                                 : QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
        m_next.attachments.insert(pa.id, TrackedEntry{pa.relPath, hash, data.size()});
        m_record.writtenAttachments++;
        return true;
    }

    void processNoteBatch() {
        if (m_canceled) {
            m_record.error = T("已取消");
            return finish(false);
        }
        constexpr int kBatch = 30;
        int n = 0;
        while (!m_notesToWrite.empty() && n < kBatch) {
            const PlannedNote pn = m_notesToWrite.front();
            m_notesToWrite.pop_front();
            if (!writeNote(pn)) return finish(false);
            m_doneWork++;
            n++;
        }
        emit progress(m_doneWork, std::max(1, m_totalWork), T("正在备份笔记…"));
        if (!m_notesToWrite.empty()) {
            QTimer::singleShot(0, this, [this] { processNoteBatch(); });
            return;
        }
        processNextAttachment();
    }

    void processNextAttachment() {
        if (m_canceled) {
            m_record.error = T("已取消");
            return finish(false);
        }
        if (m_attsToWrite.empty()) {
            finalizeSuccess();
            return finish(true);
        }
        const PlannedAttachment pa = m_attsToWrite.front();
        m_attsToWrite.pop_front();
        emit progress(m_doneWork, std::max(1, m_totalWork), T("正在备份附件…"));
        QPointer<IncrementalBackupJob> self(this);
        m_session->fileData(pa.id, [self, pa](bool ok, const QByteArray &data) {
            if (!self) return;
            if (!self->handleAttachmentData(pa, ok, data)) return self->finish(false);
            self->m_doneWork++;
            QTimer::singleShot(0, self, [self] {
                if (self) self->processNextAttachment();
            });
        });
    }

    void finalizeSuccess() {
        // Remove stale files that were previously tracked by our manifest.
        QSet<QString> keepPaths;
        for (const auto &e : m_next.notes) keepPaths.insert(e.path.toLower());
        for (const auto &e : m_next.attachments) keepPaths.insert(e.path.toLower());

        for (const auto &old : m_prev.notes) {
            if (!isSafeRelativePath(old.path) || keepPaths.contains(old.path.toLower())) continue;
            const QString abs = QDir(m_dir).filePath(old.path);
            if (QFile::exists(abs) && QFile::remove(abs)) {
                m_record.removedNotes++;
                removeEmptyAncestors(m_dir, abs);
            }
        }
        for (const auto &old : m_prev.attachments) {
            if (!isSafeRelativePath(old.path) || keepPaths.contains(old.path.toLower())) continue;
            const QString abs = QDir(m_dir).filePath(old.path);
            if (QFile::exists(abs) && QFile::remove(abs)) {
                m_record.removedAttachments++;
                removeEmptyAncestors(m_dir, abs);
            }
        }

        m_record.ok = true;
        m_record.durationMs = m_elapsed.elapsed();
        m_record.summary = formatRunSummary(m_record);
        m_next.lastBackupAt = m_record.timestamp;
        m_next.history.prepend(m_record);
        while (m_next.history.size() > kMaxHistoryEntries) m_next.history.removeLast();
        if (!saveManifest(m_dir, m_next)) {
            m_record.ok = false;
            m_record.error = T("写入备份索引文件失败。");
            m_record.summary = m_record.error;
        }
    }

    void recordFailureInManifestIfPossible() {
        if (m_dir.isEmpty() || !QDir(m_dir).exists() || !probeDirWritable(m_dir)) return;
        ManifestData existing = loadManifest(m_dir);
        if (existing.vaultId.isEmpty() && m_session) {
            existing.vaultId = m_session->info().id;
            existing.vaultName = m_session->info().name;
        }
        if (m_session && existing.vaultId != m_session->info().id) return;
        existing.history.prepend(m_record);
        while (existing.history.size() > kMaxHistoryEntries) existing.history.removeLast();
        saveManifest(m_dir, existing);
    }

    void finish(bool ok) {
        finishSync(ok);
        emit finished(m_record.ok, m_record.summary);
    }

    void finishSync(bool ok) {
        m_record.ok = ok && m_record.error.isEmpty();
        m_record.durationMs = m_elapsed.elapsed();
        m_record.summary = formatRunSummary(m_record);
        if (!m_record.ok) recordFailureInManifestIfPossible();
        if (m_outRecord) *m_outRecord = m_record;
    }

    QPointer<Session> m_session;
    QString m_dir;
    bool m_manual = true;
    RunRecord *m_outRecord = nullptr;
    QElapsedTimer m_elapsed;
    RunRecord m_record;
    ManifestData m_prev;
    ManifestData m_next;
    std::deque<PlannedNote> m_notesToWrite;
    std::deque<PlannedAttachment> m_attsToWrite;
    int m_totalWork = 0;
    int m_doneWork = 0;
};

QString settingsKey(const QString &vaultId, const char *field) {
    return QStringLiteral("backup/%1/%2").arg(vaultId, QLatin1String(field));
}

} // namespace

namespace backup {

QString safeVaultFolderName(const VaultInfo &vault) {
    const QString base = safeName(vault.name, QStringLiteral("LocalVault"));
    const QString suffix = vault.id.left(6);
    return suffix.isEmpty() ? base : QStringLiteral("%1_%2").arg(base, suffix);
}

bool isVirtualFileSystem(const QString &fsType, const QString &rootPath) {
    const QString fs = fsType.trimmed().toLower();
    static const QSet<QString> virtualFs{
        QStringLiteral("tmpfs"),      QStringLiteral("devtmpfs"),  QStringLiteral("squashfs"),
        QStringLiteral("overlay"),    QStringLiteral("overlayfs"), QStringLiteral("proc"),
        QStringLiteral("sysfs"),      QStringLiteral("cgroup"),    QStringLiteral("cgroup2"),
        QStringLiteral("ramfs"),      QStringLiteral("autofs"),    QStringLiteral("devpts"),
        QStringLiteral("efivarfs"),   QStringLiteral("debugfs"),   QStringLiteral("tracefs"),
        QStringLiteral("securityfs"), QStringLiteral("pstore"),    QStringLiteral("mqueue"),
        QStringLiteral("hugetlbfs"),  QStringLiteral("fusectl"),   QStringLiteral("configfs"),
        QStringLiteral("devfs"),      QStringLiteral("fuse.portal"), QStringLiteral("fuse.gvfsd-fuse"),
        QStringLiteral("nsfs"),       QStringLiteral("binfmt_misc"),
    };
    if (virtualFs.contains(fs)) return true;
    if (fs.startsWith(QLatin1String("cgroup"))) return true;

    const QString root = cleanDirPath(rootPath);
    if (root == QLatin1String("/boot") || root.startsWith(QLatin1String("/boot/")) ||
        root.startsWith(QLatin1String("/snap/")) || root == QLatin1String("/snap") ||
        root.startsWith(QLatin1String("/run/")) || root == QLatin1String("/run") ||
        root.startsWith(QLatin1String("/sys/")) || root == QLatin1String("/sys") ||
        root.startsWith(QLatin1String("/proc/")) || root == QLatin1String("/proc") ||
        root.startsWith(QLatin1String("/dev/")) || root == QLatin1String("/dev") ||
        root.startsWith(QLatin1String("/var/lib/docker")) ||
        root.startsWith(QLatin1String("/var/lib/containers"))) {
        return true;
    }
    // macOS sealed system volumes (keep /System/Volumes/Data and /Volumes/*)
    if (root.startsWith(QLatin1String("/System/Volumes/")) && root != QLatin1String("/System/Volumes/Data")) {
        return true;
    }
    return false;
}

VolumeCandidate pickBestVolume(const QList<VolumeCandidate> &volumes, qint64 minSpaceBytes) {
    QList<VolumeCandidate> usable;
    for (const VolumeCandidate &v : volumes) {
        if (!v.valid || !v.ready || v.readOnly || !v.writable) continue;
        if (v.candidateBaseDir.trimmed().isEmpty()) continue;
        if (isVirtualFileSystem(v.fileSystemType, v.rootPath)) continue;
        if (v.bytesTotal <= 0 && v.bytesAvailable <= 0) continue;
        usable << v;
    }
    if (usable.isEmpty()) return {};

    // Prefer volumes that meet the minimum recommended space, picking the largest disk among them.
    QList<VolumeCandidate> healthy;
    for (const VolumeCandidate &v : usable) {
        if (v.bytesAvailable >= minSpaceBytes) healthy << v;
    }
    auto &pool = healthy.isEmpty() ? usable : healthy;
    if (!healthy.isEmpty()) {
        std::sort(pool.begin(), pool.end(), [](const VolumeCandidate &a, const VolumeCandidate &b) {
            if (a.bytesTotal != b.bytesTotal) return a.bytesTotal > b.bytesTotal;
            return a.bytesAvailable > b.bytesAvailable;
        });
    } else {
        std::sort(pool.begin(), pool.end(), [](const VolumeCandidate &a, const VolumeCandidate &b) {
            if (a.bytesAvailable != b.bytesAvailable) return a.bytesAvailable > b.bytesAvailable;
            return a.bytesTotal > b.bytesTotal;
        });
    }
    return pool.first();
}

QList<VolumeCandidate> scanVolumes() {
    QList<VolumeCandidate> out;
    const QString home = cleanDirPath(QDir::homePath());
    const QStorageInfo homeVol(home);
    const QByteArray homeDev = homeVol.device();
    const QString homeRoot = cleanDirPath(homeVol.rootPath());

    QSet<QString> seenBase;
    for (const QStorageInfo &s : QStorageInfo::mountedVolumes()) {
        const QString root = cleanDirPath(s.rootPath());
        const QString fs = QString::fromLatin1(s.fileSystemType());
        if (!s.isValid() || !s.isReady() || s.isReadOnly()) continue;
        if (isVirtualFileSystem(fs, root)) continue;

        VolumeCandidate c;
        c.rootPath = root;
        c.displayName = s.displayName().isEmpty() ? root : s.displayName();
        c.fileSystemType = fs;
        c.bytesTotal = s.bytesTotal();
        c.bytesAvailable = s.bytesAvailable();
        c.valid = s.isValid();
        c.ready = s.isReady();
        c.readOnly = s.isReadOnly();

        // On the volume hosting $HOME, place backups under $HOME/IvynoteBackups so
        // root-owned "/" on Linux/macOS or ACL-restricted C:\ works reliably.
        const bool isHomeVol = !home.isEmpty() &&
                               ((!homeDev.isEmpty() && s.device() == homeDev && root == homeRoot) ||
                                (root == homeRoot));
        QString base;
        if (isHomeVol) {
            base = QDir(home).filePath(QStringLiteral("IvynoteBackups"));
        } else {
            base = QDir(root).filePath(QStringLiteral("IvynoteBackups"));
        }
        base = cleanDirPath(base);
        if (seenBase.contains(base.toLower())) continue;
        seenBase.insert(base.toLower());

        c.candidateBaseDir = base;
        c.writable = probeDirWritable(base);
        out << c;
    }
    return out;
}

QString suggestDefaultDirectory(const VaultInfo &vault) {
    const VolumeCandidate best = pickBestVolume(scanVolumes());
    QString base = best.candidateBaseDir;
    if (base.isEmpty()) {
        QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        if (docs.isEmpty()) docs = QDir::homePath();
        base = cleanDirPath(QDir(docs).filePath(QStringLiteral("IvynoteBackups")));
    }
    return cleanDirPath(QDir(base).filePath(safeVaultFolderName(vault)));
}

qint64 estimateVaultBytes(const VaultInfo &vault) {
    qint64 total = 0;
    const QString d = vault.dir();
    if (d.isEmpty() || !QDir(d).exists()) return 0;
    QDirIterator it(d, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

SpaceCheck checkSpace(const QString &dir, qint64 estimatedVaultBytes) {
    SpaceCheck sc;
    sc.recommendedMinBytes = std::max(kLowSpaceWarningBytes, estimatedVaultBytes * 2);
    sc.criticalMinBytes = std::max(kCriticalSpaceBytes, estimatedVaultBytes + 32LL * 1024 * 1024);
    const QString existing = nearestExistingAncestor(dir);
    if (existing.isEmpty()) return sc;
    const QStorageInfo info(existing);
    if (!info.isValid() || !info.isReady()) return sc;
    sc.valid = true;
    sc.mountRoot = cleanDirPath(info.rootPath());
    sc.bytesTotal = info.bytesTotal();
    sc.bytesAvailable = info.bytesAvailable();
    sc.lowSpace = sc.bytesAvailable < sc.recommendedMinBytes;
    sc.criticalSpace = sc.bytesAvailable < sc.criticalMinBytes;
    return sc;
}

bool isPathInsideVault(const QString &dir, const VaultInfo &vault) {
    const QString target = cleanDirPath(QFileInfo(dir).absoluteFilePath());
    if (target.isEmpty()) return false;
    auto isInside = [&](const QString &parent) {
        const QString p = cleanDirPath(QFileInfo(parent).absoluteFilePath());
        if (p.isEmpty()) return false;
        return target.compare(p, Qt::CaseInsensitive) == 0 ||
               target.startsWith(p + u'/', Qt::CaseInsensitive);
    };
    if (!vault.id.isEmpty() && isInside(vault.dir())) return true;
    if (isInside(vaults::dataRoot() + QStringLiteral("/vaults"))) return true;
    return false;
}

bool isDirectoryWritable(const QString &dir) {
    return probeDirWritable(dir);
}

Config loadConfig(const VaultInfo &vault) {
    Config cfg;
    cfg.enabled = !vault.isRemote();
    cfg.configured = vault.isRemote();
    if (vault.id.isEmpty()) return cfg;
    QSettings s;
    cfg.enabled = s.value(settingsKey(vault.id, "enabled"), !vault.isRemote()).toBool();
    cfg.configured = s.value(settingsKey(vault.id, "configured"), vault.isRemote()).toBool();
    cfg.directory = cleanDirPath(s.value(settingsKey(vault.id, "directory"), QString()).toString());
    cfg.intervalMinutes = std::clamp(s.value(settingsKey(vault.id, "intervalMinutes"), kDefaultIntervalMinutes).toInt(), 5, 1440);
    if (cfg.directory.isEmpty()) cfg.directory = suggestDefaultDirectory(vault);
    return cfg;
}

void saveConfig(const QString &vaultId, const Config &cfg) {
    if (vaultId.isEmpty()) return;
    QSettings s;
    s.setValue(settingsKey(vaultId, "enabled"), cfg.enabled);
    s.setValue(settingsKey(vaultId, "configured"), cfg.configured);
    s.setValue(settingsKey(vaultId, "directory"), cleanDirPath(cfg.directory));
    s.setValue(settingsKey(vaultId, "intervalMinutes"), std::clamp(cfg.intervalMinutes, 5, 1440));
}

void removeConfig(const QString &vaultId) {
    if (vaultId.isEmpty()) return;
    QSettings s;
    s.remove(QStringLiteral("backup/%1").arg(vaultId));
}

ManifestSummary readManifestSummary(const QString &dir) {
    ManifestSummary sum;
    const QString cleaned = cleanDirPath(dir);
    if (cleaned.isEmpty()) return sum;
    const QString filePath = QDir(cleaned).filePath(QLatin1String(kManifestFileName));
    if (!QFileInfo::exists(filePath)) return sum;
    const ManifestData m = loadManifest(cleaned);
    sum.exists = true;
    sum.lastBackupAt = m.lastBackupAt;
    sum.totalNotes = int(m.notes.size());
    sum.totalAttachments = int(m.attachments.size());
    sum.history = m.history;
    return sum;
}

TransferJob *exportMarkdownIncremental(Session *session, const QString &dir, bool manual,
                                       RunRecord *outRecord, QObject *parent) {
    return new IncrementalBackupJob(session, dir, manual, outRecord, parent);
}

RunRecord runIncrementalSync(Session *session, const QString &dir, bool manual) {
    IncrementalBackupJob job(session, dir, manual, nullptr, nullptr);
    return job.runSync();
}

} // namespace backup

// --- LocalBackupManager ---

LocalBackupManager::LocalBackupManager(QObject *parent) : QObject(parent) {
    connect(&m_timer, &QTimer::timeout, this, &LocalBackupManager::onScheduledTick);
}

LocalBackupManager::~LocalBackupManager() {
    m_timer.stop();
    if (m_job) m_job->cancel();
}

void LocalBackupManager::setSession(Session *session) {
    m_timer.stop();
    if (m_job) {
        m_job->cancel();
        m_job->deleteLater();
        m_job = nullptr;
    }
    if (m_session && m_session->engine()) m_session->engine()->disconnect(this);
    m_session = session;
    m_dirty = false;
    m_lowSpaceWarned = false;
    m_setupSignaled = false;
    m_config = {};

    if (!isActive()) {
        emit stateChanged();
        return;
    }

    m_config = backup::loadConfig(m_session->info());
    connect(m_session->engine(), &SyncEngine::dataChanged, this, &LocalBackupManager::onVaultDataChanged);
    resetTimer();
    emit stateChanged();

    // First time a local vault uses local backup: ask UI to show setup dialog.
    // Remote vaults default to disabled and opt in manually without a startup prompt.
    if (!m_session->isRemote() && !m_config.configured) {
        m_setupSignaled = true;
        QPointer<LocalBackupManager> self(this);
        QTimer::singleShot(450, this, [self] {
            if (self && self->isActive() && !self->m_session->isRemote() && !self->m_config.configured)
                emit self->needsInitialSetup();
        });
    } else if (m_config.enabled) {
        const backup::SpaceCheck sc = currentSpace();
        if (sc.valid && sc.lowSpace) {
            m_lowSpaceWarned = true;
            QPointer<LocalBackupManager> self(this);
            QTimer::singleShot(800, this, [self, sc] {
                if (self && self->isActive()) emit self->lowSpaceDetected(sc);
            });
        }
        // If this vault has never been backed up yet, run an initial background backup shortly.
        const backup::ManifestSummary sum = manifestSummary();
        if (!sum.exists || sum.lastBackupAt == 0) {
            QPointer<LocalBackupManager> self(this);
            QTimer::singleShot(3000, this, [self] {
                if (self && self->isActive() && self->m_config.enabled && !self->isRunning()) self->runNow(false);
            });
        }
    }
}

bool LocalBackupManager::isActive() const {
    return m_session && m_session->engine();
}

backup::ManifestSummary LocalBackupManager::manifestSummary() const {
    if (!isActive()) return {};
    return backup::readManifestSummary(m_config.directory);
}

backup::SpaceCheck LocalBackupManager::currentSpace() const {
    if (!isActive()) return {};
    return backup::checkSpace(m_config.directory, backup::estimateVaultBytes(m_session->info()));
}

void LocalBackupManager::updateConfig(const backup::Config &cfg) {
    if (!isActive()) return;
    m_config = cfg;
    m_config.intervalMinutes = std::clamp(m_config.intervalMinutes, 5, 1440);
    if (m_config.directory.trimmed().isEmpty()) m_config.directory = backup::suggestDefaultDirectory(m_session->info());
    backup::saveConfig(m_session->info().id, m_config);
    m_lowSpaceWarned = false;
    resetTimer();
    emit stateChanged();
}

void LocalBackupManager::resetTimer() {
    m_timer.stop();
    if (!isActive() || !m_config.enabled) return;
    m_timer.setInterval(std::clamp(m_config.intervalMinutes, 5, 1440) * 60 * 1000);
    m_timer.start();
}

void LocalBackupManager::onVaultDataChanged() {
    m_dirty = true;
}

void LocalBackupManager::onScheduledTick() {
    if (!isActive() || !m_config.enabled || isRunning()) return;
    const backup::ManifestSummary sum = manifestSummary();
    if (!m_dirty && sum.exists && sum.lastBackupAt > 0) return;
    runNow(false);
}

void LocalBackupManager::runNow(bool manual) {
    if (!isActive() || isRunning()) return;
    const backup::SpaceCheck sc = currentSpace();
    if (sc.valid && sc.lowSpace && (!m_lowSpaceWarned || manual)) {
        m_lowSpaceWarned = true;
        emit lowSpaceDetected(sc);
    }
    m_pendingRecord = {};
    m_job = backup::exportMarkdownIncremental(m_session, m_config.directory, manual, &m_pendingRecord, this);
    emit backupStarted(manual);
    emit stateChanged();
    connect(m_job, &TransferJob::progress, this, &LocalBackupManager::backupProgress);
    connect(m_job, &TransferJob::finished, this, [this](bool ok, const QString &) {
        if (m_job) {
            m_job->deleteLater();
            m_job = nullptr;
        }
        const backup::RunRecord rec = m_pendingRecord;
        if (ok && rec.missingAttachments == 0) m_dirty = false;
        emit backupFinished(rec);
        emit stateChanged();
    });
    m_job->start();
}

void LocalBackupManager::runIfDirtyOnClose() {
    if (!isActive() || !m_config.enabled || !m_dirty || isRunning()) return;
    if (m_config.directory.isEmpty() || backup::isPathInsideVault(m_config.directory, m_session->info())) return;
    const backup::SpaceCheck sc = currentSpace();
    if (sc.valid && sc.criticalSpace) return;
    const backup::RunRecord rec = backup::runIncrementalSync(m_session, m_config.directory, false);
    if (rec.ok && rec.missingAttachments == 0) m_dirty = false;
}

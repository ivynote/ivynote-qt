#include "vault/transfer.h"

#include "core/i18n.h"
#include "core/ids.h"
#include "core/markdown.h"
#include "sync/engine.h"
#include "vault/session.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>
#include <QUrl>
#include <deque>

namespace {

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
    for (int i = 2; taken.contains(name.toLower()); ++i) name = QStringLiteral("%1 (%2)%3").arg(base).arg(i).arg(ext);
    taken.insert(name.toLower());
    return name;
}

QString relLink(const QString &fromDir, const QString &toPath) {
    const QString rel = QDir(fromDir).relativeFilePath(toPath);
    // Encode what Markdown link syntax would break on, keep the rest readable.
    QString out;
    for (QChar c : rel) {
        if (c == u' ') out += QStringLiteral("%20");
        else if (c == u'(') out += QStringLiteral("%28");
        else if (c == u')') out += QStringLiteral("%29");
        else out += c;
    }
    return out;
}

QString isoTime(qint64 ms) { return QDateTime::fromMSecsSinceEpoch(ms).toOffsetFromUtc(QDateTime::currentDateTime().offsetFromUtc()).toString(Qt::ISODate); }

QString yamlString(const QString &s) {
    QString e = s;
    e.replace(u'\\', QStringLiteral("\\\\")).replace(u'"', QStringLiteral("\\\""));
    return u'"' + e + u'"';
}

// --- export ---

class ExportJob : public TransferJob {
public:
    ExportJob(Session *s, QString dir, QObject *parent) : TransferJob(parent), m_session(s), m_dir(std::move(dir)) {}

    void start() override {
        SyncEngine *e = m_session->engine();
        QDir().mkpath(m_dir);
        // Notebook directories, in tree order.
        QHash<QString, QString> folderDir;
        QHash<QString, QSet<QString>> taken; // dir -> names used
        std::function<QString(const QString &)> dirOf = [&](const QString &fid) -> QString {
            if (fid.isEmpty() || !e->folders().contains(fid)) return m_dir;
            if (folderDir.contains(fid)) return folderDir.value(fid);
            const Folder &f = e->folders()[fid];
            const QString parent = f.parentId == fid ? m_dir : dirOf(f.parentId);
            const QString name = uniqueIn(taken[parent], safeName(f.name, T("笔记本")), QString());
            const QString path = parent + u'/' + name;
            folderDir.insert(fid, path);
            QDir().mkpath(path);
            return path;
        };
        taken[m_dir].insert(QStringLiteral("_attachments"));
        QHash<QString, QString> notePath;
        QList<const Note *> notes;
        for (const Note *n : e->noteList())
            if (!n->trashed()) notes << n;
        for (const Note *n : notes) {
            const QString dir = dirOf(n->folderId);
            notePath.insert(n->id, dir + u'/' + uniqueIn(taken[dir], safeName(n->title, T("无标题")), QStringLiteral(".md")));
        }
        // Files referenced by exported notes.
        static const QRegularExpression res(QStringLiteral(R"(ivy://res/([A-Za-z0-9_-]{8,64}))"));
        static const QRegularExpression noteRef(QStringLiteral(R"(ivy://note/([A-Za-z0-9_-]{8,64}))"));
        QSet<QString> takenFiles;
        for (const Note *n : notes) {
            for (const QString &id : md::resourceRefs(n->body)) {
                if (m_files.contains(id)) continue;
                const QString name = uniqueIn(takenFiles, safeName(e->attachmentName(id), id), QString());
                m_files.insert(id, m_dir + QStringLiteral("/_attachments/") + name);
            }
        }
        for (const Note *n : notes) {
            const QString path = notePath.value(n->id);
            const QString dir = QFileInfo(path).absolutePath();
            QString body = n->body;
            QString out;
            qsizetype last = 0;
            for (auto it = res.globalMatch(body); it.hasNext();) {
                const auto m = it.next();
                out += body.mid(last, m.capturedStart() - last);
                out += m_files.contains(m.captured(1)) ? relLink(dir, m_files.value(m.captured(1))) : m.captured(0);
                last = m.capturedEnd();
            }
            out += body.mid(last);
            body = out;
            out.clear();
            last = 0;
            for (auto it = noteRef.globalMatch(body); it.hasNext();) {
                const auto m = it.next();
                out += body.mid(last, m.capturedStart() - last);
                out += notePath.contains(m.captured(1)) ? relLink(dir, notePath.value(m.captured(1))) : m.captured(0);
                last = m.capturedEnd();
            }
            out += body.mid(last);
            QStringList tagNames;
            for (const QString &t : n->tags)
                if (e->tags().contains(t)) tagNames << yamlString(e->tags()[t].name);
            QString front = QStringLiteral("---\ntitle: %1\ncreated: %2\nupdated: %3\n").arg(yamlString(n->title), isoTime(n->createdAt), isoTime(n->updatedAt));
            if (!tagNames.isEmpty()) front += QStringLiteral("tags: [%1]\n").arg(tagNames.join(QStringLiteral(", ")));
            if (n->pinned) front += QStringLiteral("pinned: true\n");
            front += QStringLiteral("---\n\n");
            QSaveFile f(path);
            if (f.open(QIODevice::WriteOnly)) {
                f.write((front + out).toUtf8());
                if (f.commit()) m_notes++;
            }
        }
        if (!m_files.isEmpty()) QDir().mkpath(m_dir + QStringLiteral("/_attachments"));
        m_queue.assign(m_files.keyBegin(), m_files.keyEnd());
        m_total = int(m_queue.size());
        next();
    }

private:
    void next() {
        if (m_canceled) return emit finished(false, T("已取消"));
        emit progress(m_total - int(m_queue.size()), m_total, T("正在导出附件"));
        if (m_queue.empty()) {
            QString summary = QStringLiteral("%1 %2").arg(m_notes).arg(T("篇笔记已导出"));
            if (m_missing) summary += QStringLiteral("，%1 %2").arg(m_missing).arg(T("个附件无法获取"));
            return emit finished(true, summary);
        }
        const QString id = m_queue.front();
        m_queue.pop_front();
        QPointer<ExportJob> self(this);
        m_session->fileData(id, [self, id](bool ok, const QByteArray &data) {
            if (!self) return;
            if (ok) {
                QSaveFile f(self->m_files.value(id));
                if (f.open(QIODevice::WriteOnly)) {
                    f.write(data);
                    f.commit();
                }
            } else {
                self->m_missing++;
            }
            QTimer::singleShot(0, self, [self] { if (self) self->next(); });
        });
    }

    Session *m_session;
    QString m_dir;
    QHash<QString, QString> m_files;
    std::deque<QString> m_queue;
    int m_total = 0;
    int m_notes = 0;
    int m_missing = 0;
};

// --- shared import machinery ---

struct Pending {
    NoteInit init;
    QStringList tagNames;
    // Resolved during the run: source key -> file to attach.
    QList<QPair<QString, QString>> files; // placeholder token, source key
};

class ImportBase : public TransferJob {
public:
    ImportBase(Session *s, QObject *parent) : TransferJob(parent), m_session(s) {}

protected:
    // Reads a file for a source key (a path or another vault's id).
    virtual void readFile(const QString &key, std::function<void(bool, QByteArray, QString name, QString mime)> done) = 0;

    QString tagId(const QString &name) {
        const QString n = name.trimmed().remove(QRegularExpression(QStringLiteral("^#+"))).trimmed();
        if (n.isEmpty()) return {};
        for (const Tag &t : m_session->engine()->tags())
            if (t.name.compare(n, Qt::CaseInsensitive) == 0) return t.id;
        return m_session->engine()->createTag(n.left(50)).id;
    }

    void run() {
        m_total = int(m_fileKeys.size() + m_notes.size());
        m_fileQueue.assign(m_fileKeys.begin(), m_fileKeys.end());
        nextFile();
    }

    void nextFile() {
        if (m_canceled) return emit finished(false, T("已取消"));
        if (m_fileQueue.empty()) return createNotes();
        emit progress(m_done, m_total, T("正在导入附件"));
        const QString key = m_fileQueue.front();
        m_fileQueue.pop_front();
        QPointer<ImportBase> self(this);
        readFile(key, [self, key](bool ok, const QByteArray &data, const QString &name, const QString &mime) {
            if (!self) return;
            if (!ok) {
                self->m_failedFiles++;
                self->m_done++;
                return QTimer::singleShot(0, self, [self] { if (self) self->nextFile(); });
            }
            self->m_session->engine()->addFile(data, name, mime, [self, key](AddedFile f) {
                if (!self) return;
                if (f.ok) self->m_fileIds.insert(key, f.id);
                else self->m_failedFiles++;
                self->m_done++;
                QTimer::singleShot(0, self, [self] { if (self) self->nextFile(); });
            });
        });
    }

    void createNotes() {
        SyncEngine *e = m_session->engine();
        int i = 0;
        for (Pending &p : m_notes) {
            for (const auto &[token, key] : std::as_const(p.files)) {
                const QString id = m_fileIds.value(key);
                p.init.body.replace(token, id.isEmpty() ? QStringLiteral("missing-file") : QStringLiteral("ivy://res/") + id);
            }
            for (const QString &t : std::as_const(p.tagNames)) {
                const QString id = tagId(t);
                if (!id.isEmpty() && !p.init.tags.contains(id) && p.init.tags.size() < 50) p.init.tags << id;
            }
            e->createNote(p.init);
            if (++i % 50 == 0) emit progress(m_done + i, m_total, T("正在导入笔记"));
        }
        e->persistNow();
        e->scheduleSync(0);
        QString summary = QStringLiteral("%1 %2").arg(m_notes.size()).arg(T("篇笔记已导入"));
        if (m_failedFiles) summary += QStringLiteral("，%1 %2").arg(m_failedFiles).arg(T("个附件导入失败"));
        emit finished(true, summary);
    }

    Session *m_session;
    QList<Pending> m_notes;
    QStringList m_fileKeys;
    std::deque<QString> m_fileQueue;
    QHash<QString, QString> m_fileIds; // source key -> new attachment id
    int m_total = 0;
    int m_done = 0;
    int m_failedFiles = 0;
};

// --- Markdown import ---

class MarkdownImport : public ImportBase {
public:
    MarkdownImport(Session *s, QStringList paths, QString target, QObject *parent, transfer::MarkdownSource source)
        : ImportBase(s, parent), m_paths(std::move(paths)), m_target(std::move(target)), m_source(source) {}

    void start() override {
        struct Source {
            QString path;
            QString relDir; // relative to the chosen folder, "" at its top
        };
        QList<Source> sources;
        QHash<QString, QStringList> vaultFiles;
        const bool obsidian = m_source == transfer::MarkdownSource::Obsidian;
        const bool specialized = m_source != transfer::MarkdownSource::Generic;
        for (const QString &p : m_paths) {
            const QFileInfo fi(p);
            if (fi.isDir()) {
                QDirIterator it(p, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
                while (it.hasNext()) {
                    const QString f = it.next();
                    const QFileInfo file(f);
                    if (obsidian) vaultFiles[file.fileName().toCaseFolded()].append(file.absoluteFilePath());
                    const auto ext = file.suffix().toLower();
                    if (ext != QLatin1String("md") && ext != QLatin1String("markdown") && (specialized || ext != QLatin1String("txt"))) continue;
                    QString rel = QDir(p).relativeFilePath(QFileInfo(f).absolutePath());
                    if (rel == QLatin1String(".")) rel.clear();
                    if (rel.startsWith(QLatin1String("_attachments")) || rel.contains(QLatin1String("/."))) continue;
                    if (m_source == transfer::MarkdownSource::Joplin && (rel == QStringLiteral("_resources") || rel.startsWith(QStringLiteral("_resources/")))) continue;
                    // The chosen folder itself becomes a notebook.
                    rel = rel.isEmpty() ? fi.fileName() : fi.fileName() + u'/' + rel;
                    sources << Source{f, rel};
                }
            } else if (fi.isFile()) {
                sources << Source{fi.absoluteFilePath(), QString()};
            }
        }
        std::sort(sources.begin(), sources.end(), [](const Source &a, const Source &b) { return a.path < b.path; });
        // Pre-assign ids so links between imported notes can be rewired.
        QHash<QString, QString> idOfPath;
        for (const Source &s : sources) idOfPath.insert(QFileInfo(s.path).absoluteFilePath(), ids::newId());

        static const QRegularExpression link(QStringLiteral(R"((!?)\[([^\]]*)\]\(<?([^)\s>]+)>?(\s+"[^"]*")?\))"));
        for (const Source &s : sources) {
            QFile f(s.path);
            if (!f.open(QIODevice::ReadOnly)) continue;
            QString text = QString::fromUtf8(f.readAll());
            Pending p;
            const QFileInfo fi(s.path);
            p.init.id = idOfPath.value(fi.absoluteFilePath());
            p.init.title = fi.completeBaseName();
            p.init.createdAt = fi.birthTime().isValid() ? fi.birthTime().toMSecsSinceEpoch() : fi.lastModified().toMSecsSinceEpoch();
            parseFrontMatter(text, p);
            if (obsidian || (m_source == transfer::MarkdownSource::Joplin && !p.init.title.startsWith(fi.completeBaseName())))
                p.init.title = fi.completeBaseName().left(300);
            QString scan = text;
            if (specialized) {
                static const QRegularExpression code(QStringLiteral(R"md((?:^|\n)[ \t]*(`{3,}|~{3,})[^\n]*\n[\s\S]*?(?:\n[ \t]*\1[^\n]*|$)|(`+)[^`\n]*\2)md"));
                for (auto it = code.globalMatch(text); it.hasNext();) {
                    const auto match = it.next();
                    scan.replace(match.capturedStart(), match.capturedLength(), QString(match.capturedLength(), u' '));
                }
            }
            if (obsidian) {
                static const QRegularExpression tag(QStringLiteral(R"((?:^|\s)#([\p{L}\p{N}_/-]+))"));
                for (auto it = tag.globalMatch(scan); it.hasNext();) p.tagNames << it.next().captured(1);
                static const QRegularExpression wiki(QStringLiteral(R"((!?)\[\[([^\]#|\n]+)(#[^\]|\n]*)?(?:\|([^\]\n]+))?\]\])"));
                QString rewritten, rewrittenScan;
                qsizetype offset = 0;
                for (auto it = wiki.globalMatch(scan); it.hasNext();) {
                    const auto match = it.next();
                    const QString dest = match.captured(2).trimmed();
                    auto hits = vaultFiles.value(QFileInfo(dest).fileName().toCaseFolded());
                    if (hits.isEmpty()) hits = vaultFiles.value(QFileInfo(dest).fileName().toCaseFolded() + QStringLiteral(".md"));
                    std::sort(hits.begin(), hits.end());
                    QString resolved;
                    for (const auto &candidate : hits) {
                        if (dest.contains(u'/') && !candidate.endsWith(u'/' + dest, Qt::CaseInsensitive) &&
                            !candidate.endsWith(u'/' + dest + QStringLiteral(".md"), Qt::CaseInsensitive)) continue;
                        if (resolved.isEmpty() || QFileInfo(candidate).absolutePath() == fi.absolutePath()) resolved = candidate;
                    }
                    if (resolved.isEmpty()) continue;
                    rewritten += text.mid(offset, match.capturedStart() - offset);
                    rewrittenScan += scan.mid(offset, match.capturedStart() - offset);
                    const QString label = match.captured(4).isEmpty() ? dest : match.captured(4);
                    const QString path = fi.absoluteDir().relativeFilePath(resolved);
                    const QString replacement = QStringLiteral("%1[%2](%3%4)").arg(match.captured(1), label,
                        QString::fromLatin1(QUrl::toPercentEncoding(path, "/")), match.captured(3));
                    rewritten += replacement;
                    rewrittenScan += replacement;
                    offset = match.capturedEnd();
                }
                text = rewritten + text.mid(offset);
                scan = rewrittenScan + scan.mid(offset);
            }
            p.init.folderId = folderFor(s.relDir);
            // Rewrite local links: notes we import, files we attach.
            QString out;
            qsizetype last = 0;
            for (auto it = link.globalMatch(scan); it.hasNext();) {
                const auto m = it.next();
                out += text.mid(last, m.capturedStart() - last);
                last = m.capturedEnd();
                const QString target = m.captured(3);
                if (QUrl(target).scheme().size() > 1 || target.startsWith(u'#')) {
                    out += m.captured(0);
                    continue;
                }
                const QString path = QDir::cleanPath(QFileInfo(fi.absoluteDir(), QUrl::fromPercentEncoding(target.split(u'#').first().toUtf8())).absoluteFilePath());
                if (specialized && !QFileInfo(path).canonicalFilePath().startsWith(QFileInfo(m_paths.first()).canonicalFilePath() + u'/')) {
                    out += m.captured(0);
                    continue;
                }
                if (idOfPath.contains(path)) {
                    out += QStringLiteral("%1[%2](ivy://note/%3)").arg(m.captured(1), m.captured(2), idOfPath.value(path));
                } else if (QFileInfo(path).isFile()) {
                    if (!m_fileKeys.contains(path)) m_fileKeys << path;
                    const QString token = QStringLiteral("\x01file:%1\x01").arg(m_fileKeys.indexOf(path));
                    p.files.append({token, path});
                    out += QStringLiteral("%1[%2](%3)").arg(m.captured(1), m.captured(2), token);
                } else {
                    out += m.captured(0);
                }
            }
            out += text.mid(last);
            p.init.body = out;
            m_notes << p;
        }
        run();
    }

protected:
    void readFile(const QString &key, std::function<void(bool, QByteArray, QString, QString)> done) override {
        QFile f(key);
        const bool ok = f.open(QIODevice::ReadOnly);
        const QByteArray data = ok ? f.readAll() : QByteArray();
        const QString mime = QMimeDatabase().mimeTypeForFileNameAndData(key, data).name();
        QTimer::singleShot(0, this, [done, ok, data, key, mime] { done(ok, data, QFileInfo(key).fileName(), mime); });
    }

private:
    void parseFrontMatter(QString &text, Pending &p) {
        if (text.startsWith(QChar(0xFEFF))) text.remove(0, 1);
        if (!text.startsWith(QLatin1String("---\n")) && !text.startsWith(QLatin1String("---\r\n"))) return;
        const qsizetype end = text.indexOf(QRegularExpression(QStringLiteral("\\n---\\s*(\\n|$)")), 3);
        if (end < 0) return;
        const QString yaml = text.mid(4, end - 4);
        const qsizetype bodyStart = text.indexOf(u'\n', end + 1);
        text = bodyStart < 0 ? QString() : text.mid(bodyStart + 1);
        while (text.startsWith(u'\n')) text.remove(0, 1);
        QString listKey;
        for (const QString &raw : yaml.split(u'\n')) {
            const QString line = raw.trimmed();
            if (line.startsWith(QLatin1String("- ")) && !listKey.isEmpty()) {
                if (listKey == QLatin1String("tags")) p.tagNames << unquote(line.mid(2));
                continue;
            }
            const qsizetype colon = line.indexOf(u':');
            if (colon <= 0) continue;
            const QString key = line.left(colon).trimmed().toLower();
            const QString value = line.mid(colon + 1).trimmed();
            listKey.clear();
            if (key == QLatin1String("title")) {
                if (!value.isEmpty()) p.init.title = unquote(value);
            } else if (key == QLatin1String("created") || key == QLatin1String("date")) {
                if (const qint64 t = parseTime(value)) p.init.createdAt = t;
            } else if (key == QLatin1String("tags") || key == QLatin1String("tag") || key == QLatin1String("keywords")) {
                if (value.isEmpty()) listKey = QStringLiteral("tags");
                QString v = value;
                if (v.startsWith(u'[')) v = v.mid(1, v.size() - 2);
                for (const QString &t : v.split(u',', Qt::SkipEmptyParts)) p.tagNames << unquote(t.trimmed());
            } else if (key == QLatin1String("pinned")) {
                p.init.pinned = value == QLatin1String("true");
            }
        }
        p.init.title = p.init.title.simplified().left(300);
    }

    static QString unquote(QString s) {
        s = s.trimmed();
        if (s.size() >= 2 && ((s.startsWith(u'"') && s.endsWith(u'"')) || (s.startsWith(u'\'') && s.endsWith(u'\'')))) s = s.mid(1, s.size() - 2);
        s.replace(QStringLiteral("\\\""), QStringLiteral("\""));
        return s;
    }

    static qint64 parseTime(const QString &v) {
        QDateTime d = QDateTime::fromString(unquote(v), Qt::ISODate);
        if (!d.isValid()) d = QDateTime::fromString(unquote(v), QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        if (!d.isValid()) d = QDateTime::fromString(unquote(v), QStringLiteral("yyyy-MM-dd"));
        return d.isValid() ? d.toMSecsSinceEpoch() : 0;
    }

    QString folderFor(const QString &relDir) {
        if (relDir.isEmpty()) return m_target;
        if (m_folders.contains(relDir)) return m_folders.value(relDir);
        const qsizetype slash = relDir.lastIndexOf(u'/');
        const QString parent = slash < 0 ? m_target : folderFor(relDir.left(slash));
        const QString name = (slash < 0 ? relDir : relDir.mid(slash + 1)).left(100);
        SyncEngine *e = m_session->engine();
        // Merge into an existing notebook of the same name.
        for (const Folder &f : e->folders()) {
            if (f.parentId == parent && f.name == name) {
                m_folders.insert(relDir, f.id);
                return f.id;
            }
        }
        const QString id = e->createFolder(name, parent).id;
        m_folders.insert(relDir, id);
        return id;
    }

    QStringList m_paths;
    QString m_target;
    transfer::MarkdownSource m_source;
    QHash<QString, QString> m_folders;
};

// --- vault import ---

class VaultImport : public ImportBase {
public:
    VaultImport(Session *target, VaultInfo source, QObject *parent) : ImportBase(target, parent), m_source(std::move(source)) {}
    ~VaultImport() override { delete m_from; }

    void start() override {
        Session::OpenError err;
        QString msg;
        m_from = Session::open(m_source, {}, &err, &msg, nullptr);
        if (!m_from) {
            QTimer::singleShot(0, this, [this] { emit finished(false, T("无法打开来源笔记库")); });
            return;
        }
        SyncEngine *src = m_from->engine();
        SyncEngine *dst = m_session->engine();
        // Notebooks, parents first, under a notebook named after the source.
        const QString root = dst->createFolder(m_source.name.left(100)).id;
        QHash<QString, QString> folderIds;
        std::function<QString(const QString &, int)> mapFolder = [&](const QString &fid, int depth) -> QString {
            if (fid.isEmpty() || !src->folders().contains(fid) || depth > 64) return root;
            if (folderIds.contains(fid)) return folderIds.value(fid);
            const Folder &f = src->folders()[fid];
            const QString parent = mapFolder(f.parentId, depth + 1);
            const QString id = dst->createFolder(f.name, parent).id;
            folderIds.insert(fid, id);
            return id;
        };
        QList<const Folder *> folders = src->folderList();
        std::sort(folders.begin(), folders.end(), [](const Folder *a, const Folder *b) { return a->sort < b->sort; });
        for (const Folder *f : folders) mapFolder(f->id, 0);

        QHash<QString, QString> noteIds;
        QList<const Note *> notes;
        for (const Note *n : src->noteList())
            if (!n->trashed()) {
                notes << n;
                noteIds.insert(n->id, ids::newId());
            }
        static const QRegularExpression res(QStringLiteral(R"(ivy://res/([A-Za-z0-9_-]{8,64}))"));
        static const QRegularExpression noteRef(QStringLiteral(R"(ivy://note/([A-Za-z0-9_-]{8,64}))"));
        for (const Note *n : notes) {
            Pending p;
            p.init.id = noteIds.value(n->id);
            p.init.title = n->title;
            p.init.folderId = mapFolder(n->folderId, 0);
            p.init.pinned = n->pinned;
            p.init.createdAt = n->createdAt;
            for (const QString &t : n->tags)
                if (src->tags().contains(t)) p.tagNames << src->tags()[t].name;
            QString body = n->body;
            QString out;
            qsizetype last = 0;
            for (auto it = noteRef.globalMatch(body); it.hasNext();) {
                const auto m = it.next();
                out += body.mid(last, m.capturedStart() - last);
                out += noteIds.contains(m.captured(1)) ? QStringLiteral("ivy://note/") + noteIds.value(m.captured(1)) : m.captured(0);
                last = m.capturedEnd();
            }
            out += body.mid(last);
            body = out;
            out.clear();
            last = 0;
            for (auto it = res.globalMatch(body); it.hasNext();) {
                const auto m = it.next();
                out += body.mid(last, m.capturedStart() - last);
                const QString key = m.captured(1);
                if (!m_fileKeys.contains(key)) m_fileKeys << key;
                const QString token = QStringLiteral("\x01res:%1\x01").arg(key);
                p.files.append({token, key});
                out += token;
                last = m.capturedEnd();
            }
            out += body.mid(last);
            p.init.body = out;
            m_notes << p;
        }
        run();
    }

protected:
    void readFile(const QString &key, std::function<void(bool, QByteArray, QString, QString)> done) override {
        SyncEngine *src = m_from->engine();
        QString mime;
        if (key.startsWith(QLatin1String("local-"))) mime = src->uploads().value(key).mime;
        else mime = src->attachments().value(key).mime;
        const QString name = src->attachmentName(key);
        m_from->fileData(key, [done, name, mime](bool ok, const QByteArray &data) {
            done(ok, data, name.isEmpty() ? QStringLiteral("file") : name, mime.isEmpty() ? QStringLiteral("application/octet-stream") : mime);
        });
    }

private:
    VaultInfo m_source;
    Session *m_from = nullptr;
};

} // namespace

namespace transfer {

TransferJob *exportMarkdown(Session *session, const QString &dir, QObject *parent) { return new ExportJob(session, dir, parent); }

TransferJob *importMarkdown(Session *session, const QStringList &paths, const QString &targetFolderId, QObject *parent, MarkdownSource source) {
    return new MarkdownImport(session, paths, targetFolderId, parent, source);
}

TransferJob *importVault(Session *target, const VaultInfo &source, QObject *parent) { return new VaultImport(target, source, parent); }

} // namespace transfer
